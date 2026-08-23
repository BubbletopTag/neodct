/* nd_keycodes.h -- the one keycode namespace the whole OS shares.
 *
 * NeoDCT keycodes ARE Linux evdev keycodes. That is why the uinput bridge
 * needs no translation table for the navigation keys, and why the two
 * deliberate abuses below cost nothing: '*' is KEY_LEFTSHIFT (42) and '#' is
 * KEY_BACKSLASH (43). Port them as-is; the hardware keymap, the T9 engine,
 * both games and every app agree on them.
 */

#ifndef ND_KEYCODES_H_INCLUDED
#define ND_KEYCODES_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "no key" from every read that can time out. Chosen negative so it can never
 * collide with a real evdev code. */
#define ND_KEY_NONE (-1)

/* The core's read path returns this instead of a keycode where the Python
 * raised IncomingCall. Only the core ever sees it -- an app gets SIGTERM. */
#define ND_KEY_INCOMING_CALL (-2)

#define ND_KEY_MINUS   12
#define ND_KEY_CLEAR   14 /* KEY_BACKSPACE; "clear" and "back" both map here */
#define ND_KEY_BACK    14
#define ND_KEY_ENTER   28 /* KEY_ENTER; "navikey" and "enter" both map here  */
#define ND_KEY_NAVIKEY 28
#define ND_KEY_C       46 /* KEY_C -- a CrashHandler continue key            */
#define ND_KEY_MENU    50 /* KEY_M, doubling as the menu key                 */
#define ND_KEY_M       50
#define ND_KEY_COMMA   51
#define ND_KEY_DOT     52
#define ND_KEY_SPACE   57
#define ND_KEY_KPENTER 96
#define ND_KEY_UP      103
#define ND_KEY_LEFT    105
#define ND_KEY_RIGHT   106
#define ND_KEY_DOWN    108

#define ND_KEY_STAR 42 /* KEY_LEFTSHIFT, used as '*'  */
#define ND_KEY_HASH 43 /* KEY_BACKSLASH, used as '#'  */

/* The number row. Note 0 is 11, after 9 -- evdev's layout, not arithmetic. */
#define ND_KEY_1 2
#define ND_KEY_2 3
#define ND_KEY_3 4
#define ND_KEY_4 5
#define ND_KEY_5 6
#define ND_KEY_6 7
#define ND_KEY_7 8
#define ND_KEY_8 9
#define ND_KEY_9 10
#define ND_KEY_0 11

/* True for ND_KEY_1..ND_KEY_0, i.e. codes 2..11 inclusive. VerticalList's
 * digit shortcut uses 2..10 only (1..9); check that range explicitly there. */
static inline bool nd_key_is_digit(int32_t code) ND_UNUSED_FN;
static inline bool nd_key_is_digit(int32_t code)
{
    return code >= ND_KEY_1 && code <= ND_KEY_0;
}

/* '1'..'9','0' for codes 2..11, otherwise 0. */
char nd_key_digit_char(int32_t code);

/* ------------------------------------------------------------------ *
 * The two character tables, which are NOT the same table
 * ------------------------------------------------------------------ */

/* NeoDCT_UI.DEV_KEYMAP (core/main.py) -- what a key DIALS on the home screen.
 * Digits, plus 12:'-' 52:'.' 51:',' 42:'*' 43:'#' and 28:'#'.
 *
 * The 28:'#' entry is dead code: handle_input consumes ENTER in an earlier
 * branch and never reaches the table. It is reproduced anyway, because "port
 * the bug too" and because a future edit to that branch would expose it. */
char nd_key_dial_char(int32_t code);

/* TextInput.DEV_KEYMAP / TextInputLong.DEV_KEYMAP (ui/framework.py) -- what a
 * key TYPES on a development QWERTY keyboard. Digits, the whole letter
 * layout, 57:' ' 52:'.' 51:',' 12:'-'. No '*' and no '#': on a QWERTY host
 * those two codes are shift and backslash and are not typeable characters.
 *
 * Returns 0 for an unmapped code, which the widgets treat as "ignore". */
char nd_key_dev_char(int32_t code);

/* MATRIX_NAME_TO_CODE, used when parsing /NeoDCT/User/keymap.json:
 *   navikey 28  clear 14  up 103  down 108  left 105  right 106
 *   menu 50     enter 28  back 14
 *   num_1..num_9 = 2..10, num_0 = 11, star 42, hash 43
 * Two names alias two codes on purpose. Returns -1 for an unknown name. */
int32_t nd_keycode_for_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ND_KEYCODES_H_INCLUDED */

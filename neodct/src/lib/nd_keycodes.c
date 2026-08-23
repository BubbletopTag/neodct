/* nd_keycodes.c -- the four lookup tables that share one keycode namespace.
 *
 * NeoDCT keycodes ARE Linux evdev keycodes, so there is no translation to do
 * anywhere; these tables answer four different questions ABOUT a code, and
 * they are deliberately not the same table:
 *
 *   nd_key_digit_char   which keypad digit is this?      (T9, dialer)
 *   nd_key_dial_char    what does this DIAL?             (home screen)
 *   nd_key_dev_char     what does this TYPE on QWERTY?   (dev keyboard)
 *   nd_keycode_for_name what code does keymap.json mean? (matrix keypad)
 *
 * Linear scans over tables of at most 40 entries. A switch would compile to
 * the same thing and reads worse next to the Python dict it is ported from.
 */

#include "nd_keycodes.h"

#include <string.h>

char nd_key_digit_char(int32_t code)
{
    /* evdev's number row: 1..9 are 2..10 and 0 is 11, AFTER 9. Not
     * arithmetic -- it is the physical order of the keys on a keyboard. */
    static const char digits[] = "1234567890";

    if (!nd_key_is_digit(code))
        return '\0';
    return digits[code - ND_KEY_1];
}

char nd_key_dial_char(int32_t code)
{
    /* NeoDCT_UI.DEV_KEYMAP, core/main.py:529.
     *
     * The 28 -> '#' entry is dead code: handle_input consumes ENTER in an
     * earlier branch and never reaches the table. Reproduced because
     * CODING-STANDARDS.md section 9.4 says to port the quirk too, and because
     * a future edit to that branch would expose it. */
    switch (code) {
    case ND_KEY_MINUS:
        return '-';
    case ND_KEY_DOT:
        return '.';
    case ND_KEY_COMMA:
        return ',';
    case ND_KEY_STAR:
        return '*';
    case ND_KEY_HASH:
        return '#';
    case ND_KEY_ENTER:
        return '#';
    default:
        return nd_key_digit_char(code);
    }
}

char nd_key_dev_char(int32_t code)
{
    /* TextInput.DEV_KEYMAP / TextInputLong.DEV_KEYMAP, ui/framework.py:675
     * and :817 -- byte-identical to each other, so one table serves both.
     *
     * No '*' and no '#': on a QWERTY host those codes are left-shift and
     * backslash, and neither is a character the widget may insert. */
    static const struct {
        int32_t code;
        char ch;
    } table[] = {
        {16, 'q'}, {17, 'w'}, {18, 'e'}, {19, 'r'}, {20, 't'}, {21, 'y'}, {22, 'u'}, {23, 'i'},
        {24, 'o'}, {25, 'p'}, {30, 'a'}, {31, 's'}, {32, 'd'}, {33, 'f'}, {34, 'g'}, {35, 'h'},
        {36, 'j'}, {37, 'k'}, {38, 'l'}, {44, 'z'}, {45, 'x'}, {46, 'c'}, {47, 'v'}, {48, 'b'},
        {49, 'n'}, {50, 'm'}, {57, ' '}, {52, '.'}, {51, ','}, {12, '-'},
    };
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(table); i++) {
        if (table[i].code == code)
            return table[i].ch;
    }
    return nd_key_digit_char(code);
}

int32_t nd_keycode_for_name(const char *name)
{
    /* MATRIX_NAME_TO_CODE, core/main.py:47. Two pairs of names alias one code
     * each on purpose -- the wizard enrols "navikey" and "clear", while a
     * hand-written keymap is more likely to say "enter" and "back". The map
     * is name -> code, so duplicates on the value side cost nothing. */
    static const struct {
        const char *name;
        int32_t code;
    } table[] = {
        {"navikey", 28}, {"clear", 14}, {"up", 103},  {"down", 108}, {"left", 105}, {"right", 106},
        {"menu", 50},    {"enter", 28}, {"back", 14}, {"num_1", 2},  {"num_2", 3},  {"num_3", 4},
        {"num_4", 5},    {"num_5", 6},  {"num_6", 7}, {"num_7", 8},  {"num_8", 9},  {"num_9", 10},
        {"num_0", 11},   {"star", 42},  {"hash", 43},
    };
    size_t i;

    if (name == NULL)
        return -1;
    for (i = 0u; i < ND_ARRAY_LEN(table); i++) {
        if (strcmp(table[i].name, name) == 0)
            return table[i].code;
    }
    return -1;
}

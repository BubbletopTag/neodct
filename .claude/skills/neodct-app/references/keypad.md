# The keypad

## What the phone actually has

Sixteen keys. The authoritative list is `nd_kpsetup_targets[]` in
`neodct/src/lib/nd_keypadsetup.c` -- it is the enrolment order of the
first-boot wizard, so it is by definition every key the hardware has:

    navikey  clear  up  down
    num_1 num_2 num_3 num_4 num_5 num_6 num_7 num_8 num_9 num_0
    star  hash

**There is no left and no right.** This is the constraint that breaks the most
designs, because `nd_keycodes.h` defines `ND_KEY_LEFT` (105) and
`ND_KEY_RIGHT` (106) and nothing stops you using them -- they simply never
arrive on the phone. They exist for the QEMU development keyboard, and every
test in the suite runs on that keyboard, so a screen that depends on them
passes every test and is unusable on the device.

If a screen needs a second axis, take it from the number pad and treat
Left/Right as an alias for the dev keyboard's comfort.

## Codes

NeoDCT keycodes ARE Linux evdev codes, which is why the uinput bridge needs no
translation table. Two deliberate abuses: `*` is `KEY_LEFTSHIFT` (42) and `#`
is `KEY_BACKSLASH` (43).

| name | code | note |
| --- | --- | --- |
| `ND_KEY_ENTER` / `ND_KEY_NAVIKEY` | 28 | the one softkey |
| `ND_KEY_CLEAR` / `ND_KEY_BACK` | 14 | clear and back are the same key |
| `ND_KEY_UP` / `ND_KEY_DOWN` | 103 / 108 | the rocker |
| `ND_KEY_1` .. `ND_KEY_9` | 2..10 | note 0 is **11**, after 9 -- evdev order |
| `ND_KEY_0` | 11 | |
| `ND_KEY_STAR` / `ND_KEY_HASH` | 42 / 43 | |
| `ND_KEY_NONE` | -1 | returned by any read that times out |
| `ND_KEY_INCOMING_CALL` | -2 | core only; an app gets SIGTERM instead |

`nd_key_is_digit()` covers 2..11. `VerticalList`'s digit shortcut deliberately
uses 2..10 only (1..9), so check that range explicitly if you copy it.

## The d-pad convention

Established by MusicPlayer and Messages, and worth following so the phone feels
consistent:

    2 = up      8 = down      4 = left      6 = right

Up/Down alias 2/8, and Left/Right alias 4/6. For a grid, the natural extension
adds month/year paging on the outer rows, which gives a complete 3x3 block:

    1  prev month    2  up          3  next month
    4  prev day      5  today       6  next day
    7  prev year     8  down        9  next year

with `*` and `#` duplicating 1 and 3 because they are the outer pair and a
thumb finds them without looking. `apps/Calendar/month.c` implements exactly
this; `nd_cal_month_key()` is the whole map as one pure function.

## Making a key map testable

Put the map in a pure function that takes a key and a cursor and returns an
enum. The test machine has neither the keypad nor a way to simulate somebody
using one, so a function is the only thing that can be checked -- and the check
worth writing is the one that walks *only the sixteen real codes* and asserts
every direction is still reachable. That is the test that catches a future edit
moving something onto Left or Right, which nothing else in the suite would
notice.

    nd_cal_nav nd_cal_month_key(int32_t key, int32_t *y, int32_t *m, int32_t *d);

Return "moved" even when the cursor hit a boundary and did not actually move,
so that holding a key at the end of a range is a steady screen rather than a
dead one. `nd_vlist_show()` makes the same choice at the ends of a list.

## Auto-repeat

Do not implement it. `nd_input` synthesises repeats for codes 103/105/106/108
from its own held state (400 ms, then every 120 ms). A held arrow arrives at
your loop as an ordinary stream of presses. A widget that derived repeat itself
would fight that layer and behave differently on the i2c keypad from under
QEMU.

## T9

Multi-tap, predictive, the `#` mode cycle and the mode indicator run only when
`ui->has_matrix_keypad` is true, which on a QWERTY dev keyboard is false. Set
`NEODCT_T9=1` to exercise them under QEMU. Note the distinction in `nd_app.h`:
`ui->has_matrix_keypad` answers "should keys mean T9", while
`nd_app_keypad_is_matrix()` answers "is the console's keyboard missing" -- they
are different questions and folding them together starts a uinput bridge
alongside a real keyboard.

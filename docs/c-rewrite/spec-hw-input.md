# Hardware layer: keypad, T9, display daemon — C port specification

*Survey of `neodct/overlay/NeoDCT/System/hw/` plus the input half of
`System/core/main.py` and the host build tool `neodct/tools/mkt9dict.py`.*

*Everything below was read from the actual source. Line references are to the files as
they exist at survey time. Where a number looks arbitrary it is quoted verbatim and is
load-bearing — port it as written.*

---

## What this does (plain English, for a reader who is not a C programmer)

This subsystem is everything between **a finger on a rubber key** and **light coming out
of the screen**. It has five separate jobs that happen to live in the same folder.

### 1. Reading the keypad

The phone has no keyboard. It has a 4×4-ish rubber matrix wired to a **PCF8575** — a
tiny chip with sixteen pins that talks over a two-wire I²C bus. The chip cannot tell you
"key 7 is down". All it can do is: *set these sixteen pins high or low*, and *tell me
what all sixteen pins read right now*.

So we scan. We pull one pin low, read all sixteen, and see which other pin went low with
it. A key is just a switch joining one "row" pin to one "column" pin, so if row pin 1
and column pin 5 read low together, the key at that crossing is pressed. Do that for
every row pin in turn, sixteen times a second, and you have a keypad.

`pcf8575_keypad.py` is that scanner. Around 234 lines, no libraries, just `open()`,
`read()`, `write()` and one `ioctl` to say "talk to the chip at address 0x20".

### 2. Working out which key is which — the first-boot wizard

Here is the chicken-and-egg problem. To *use* the keypad you need a map saying
"row 2, column 3 = the number 5 key". But that map depends on how somebody soldered
the thing, and you cannot ask the user to type it in — the keypad is the only input
device and it does not work yet.

`i2c_keypad_setup.py` solves this by drawing directly on the screen. On a fresh phone
with no map file it puts up **"Press: NaviKey (center)"**, waits for any key, records
which two pins it joined, then **"Press: C (clear/back)"**, and so on through all
sixteen keys. Then it works out mathematically which pins are rows and which are
columns (it 2-colours a graph — if pin A and pin B are ever joined by a key, they must
be on opposite sides), writes `/NeoDCT/User/keymap.json`, and restarts the UI.

Because it draws to the screen with no UI framework loaded, it is 423 lines that own
their own fonts and their own progress bar.

### 3. T9 — typing words on twelve keys

The number 7 key has to produce `p`, `q`, `r`, `s` and `7`. Old phones did this two
ways, and NeoDCT does both.

**Multi-tap:** press 7 once for `p`, twice quickly for `q`, three times for `r`.
"Quickly" is one second. Press a different key, or wait too long, and the letter is
committed and you start again. `t9_engine.py` is that logic — 203 lines of pure
bookkeeping with no screen and no files.

**Predictive:** press 4-3-5-5-6 and the phone guesses "hello" without you tapping each
letter. This needs a dictionary: 315,752 English words in a plain text file, sorted not
alphabetically but *by the digits you press to type them*. `t9_dict.py` searches it —
and crucially, it **never loads the file**. It seeks to the middle, reads the word it
landed on, works out that word's digits, and halves the search. About seventeen seeks
for a 3 MB file, and the phone's memory never sees more than one line at a time. On a
64 MB phone that is the entire reason the file exists in this format.

`mkt9dict.py` builds it, on a desktop, from two word lists. It sorts by digits, and
within one digit sequence by how common the word is — so `228` offers "cat" before
"bat" before "abt". Getting that order wrong makes the feature worse than useless.

### 4. Pretending to be a USB keyboard

Two programs on the phone are not NeoDCT and expect a real keyboard: the Linux shell,
and the NetSurf web browser. `t9_uinput.py` creates a **fake keyboard** using a Linux
feature called `uinput` — the kernel lets you invent an input device and post key
presses into it, and everything else on the system believes it is real hardware.

So keypad → T9 engine → letter → fake keyboard key press → shell sees a letter typed.
For the browser it is different: the keypad has no arrow keys at all, so 2/4/6/8 become
Up/Left/Right/Down and 5 becomes Enter, and `#` toggles between "arrows" and "typing".

### 5. Getting pixels onto the panel

The screen is a 240×240 **ST7789** wired over SPI. Linux has no driver for it that
works on this board (the kernel one, fbtft, was tried and abandoned). So there is a
small background program, `neodctDisplay.c`, that:

- creates nothing — it reads Linux's *virtual* framebuffer `/dev/fb0`, which the UI
  draws into as if it were a real screen,
- compares this frame with the last one and, if nothing changed, does nothing at all
  (this is why an idle phone does not peg its single CPU core),
- if something did change, works out the smallest rectangle containing the change,
  converts just that rectangle to the panel's colour format, and pushes it over SPI.

**This file is already C.** It is 654 lines and it works. The rewrite does not need to
touch it.

One geometry fact that matters everywhere: the UI is **240 wide by 175 tall**, not
240×240. The daemon forces the framebuffer to that size and paints the band at the
**bottom** of the physical panel (`y = 65`), leaving the top 65 rows black — matching
the window in the Nokia faceplate.

### Also in this folder

`backlight.py` turns the screen light on and off (three tiers: real dimming through
PWM, on/off through a GPIO pin, or nothing at all), and `neodct-sdcard` is a shell
script that mounts an inserted SD card. The shell script is already not Python and
needs no porting.

---

## Files and where they go in C

| Python file | LOC | What it is | C destination |
| --- | ---: | --- | --- |
| `System/hw/t9_engine.py` | 203 | multi-tap / predictive state machine, pure logic | `libneodct.so` → `nd_t9_engine.c/.h` |
| `System/hw/t9_dict.py` | 161 | on-disk binary search over the word list | `libneodct.so` → `nd_t9_dict.c/.h` |
| `System/hw/t9_uinput.py` | 354 | `/dev/uinput` virtual keyboard + shell/browser bridges | `libneodct.so` → `nd_uinput.c/.h`, `nd_t9_bridge.c/.h` |
| `System/hw/pcf8575_keypad.py` | 234 | raw-I²C PCF8575 + matrix scanner + input backend | core → `nd_pcf8575.c/.h`, `nd_matrix.c/.h` |
| `System/hw/i2c_keypad_setup.py` | 423 | first-boot enrolment wizard, draws to fb | core → `nd_keypad_setup.c/.h` |
| `System/hw/backlight.py` | 165 | 3-tier backlight control via sysfs | `libneodct.so` → `nd_backlight.c/.h` |
| `System/hw/neodctDisplay.c` | 654 | **already C** — ST7789 SPI daemon | keep as `neodct_displayd`, standalone binary |
| `System/hw/neodct-sdcard` | 274 | **already busybox sh** — SD card mount helper | keep verbatim |
| `System/core/main.py` lines 36–67, 88–195, 197–288, 1191–1233 | ~250 | keycode table, evdev discovery, keymap JSON load, gpiozero matrix backend, `read_keypress()` evdev path | core → `nd_keycodes.h`, `nd_evdev.c/.h`, `nd_keymap.c/.h`, `nd_input.c/.h` |
| `neodct/tools/mkt9dict.py` | 221 | **host build tool** — builds `t9.dict` | **stays Python.** Never runs on the phone. |
| `System/engineering/tools/consolei2ckeypadbuilder.py` | 322 | serial-console version of the wizard | **defer** — engineering tool, port last or drop |
| `System/engineering/apps/KeypadMapperI2C/main.py` | 300+ | in-UI version of the wizard | **defer** — engineering app |

Total Python in scope: **≈ 2,011 LOC** (excluding the engineering tools, which are
listed as deferred and add ~620 more).

### Data files that travel with this subsystem

| Path | Size | Notes |
| --- | ---: | --- |
| `/NeoDCT/System/core/t9.dict` | 3,022,855 bytes, 315,752 lines | read-only, in the squashfs, **never loaded into RAM** |
| `/NeoDCT/User/keymap.json` | ~2 KB | written by the wizard, on the only writable partition |
| `/NeoDCT/System/ui/resources/fonts/font.ttf` | — | wizard uses sizes 26 / 18 / 14 |

---

## Behaviour that must be reproduced exactly

### 1. The keycode vocabulary

There is one keycode namespace across the whole OS. **NeoDCT keycodes are Linux evdev
keycodes**, which is why the uinput bridge needs no translation table for nav keys. Two
of them are deliberate abuses: `*` is 42 (`KEY_LEFTSHIFT`) and `#` is 43
(`KEY_BACKSLASH`). Port them as-is.

From `System/core/main.py:47` (`MATRIX_NAME_TO_CODE`):

```
navikey 28   clear 14   up 103   down 108   left 105   right 106   menu 50
enter   28   back  14
num_1    2   num_2  3   num_3  4   num_4   5   num_5    6
num_6    7   num_7  8   num_8  9   num_9  10   num_0   11
star    42   hash  43
```

Note `navikey`/`enter` are both 28 and `clear`/`back` are both 14 — the map is
name→code, so duplicates on the value side are fine.

From `t9_engine.py:39` (`CODE_TO_DIGIT`), the inverse for digits only:

```
2→"1"  3→"2"  4→"3"  5→"4"  6→"5"  7→"6"  8→"7"  9→"8"  10→"9"  11→"0"
KEY_STAR = 42   KEY_HASH = 43
```

From `t9_uinput.py:41` (Linux codes the bridge emits):

```
KEY_BACKSPACE 14   KEY_ENTER 28   KEY_LEFTSHIFT 42
KEY_UP 103   KEY_LEFT 105   KEY_RIGHT 106   KEY_DOWN 108
PASSTHROUGH_CODES = (28, 14, 103, 105, 106, 108)
```

### 2. `nd_t9_engine` — multi-tap and predictive state machine

Ported from `t9_engine.py`. **Pure logic. No I/O, no allocation, no clock of its own —
the clock is injectable** (`clock=` argument; tests pass a fake).

#### Constants (verbatim)

```c
#define ND_T9_MODE_WORD   "word"   /* predictive */
#define ND_T9_MODE_ABC    "abc"
#define ND_T9_MODE_UPPER  "ABC"
#define ND_T9_MODE_123    "123"

#define ND_T9_FILTER_ANY      "any"
#define ND_T9_FILTER_LETTERS  "letters"
#define ND_T9_FILTER_NUMBERS  "numbers"
```

Letter cycles (`LETTER_CYCLES`, `t9_engine.py:46`):

```
"2"→"abc"  "3"→"def"  "4"→"ghi"  "5"→"jkl"
"6"→"mno"  "7"→"pqrs" "8"→"tuv"  "9"→"wxyz"
```

Key-1 punctuation cycle (`PUNCT_CYCLE`, 21 chars, exact order):

```
.,?!'"1-()@/:_;+#*=<>
```

`PUNCT_CYCLE_LETTERS` is the same string with digits removed → 20 chars:

```
.,?!'"-()@/:_;+#*=<>
```

Mode cycles per filter (`_MODES_BY_FILTER`), in the order `#` walks them:

| filter | cycle |
| --- | --- |
| `any` | `word`, `abc`, `ABC`, `123` |
| `letters` | `word`, `abc`, `ABC` |
| `numbers` | `123` (single entry) |

`_NUMBERS_CHARS = "0123456789*#+"`.

Default timeout **1.0 s**, clock is `time.monotonic()`.

#### Start state

```
_mode_idx = index of MODE_ABC in the filter's cycle, or 0 if abc is absent
```

So `any` and `letters` start at **`abc`** (index 1), `numbers` starts at `123`
(index 0). Predictive sits *before* abc in the cycle but is never the start mode.
`_pending_digit = NULL`, `_pending_idx = 0`, `_word_digits = ""`, `_last_press = 0.0`.

#### `press(code)` — exact decision order

The returned op is a `(kind, value)` pair. Kinds: `append`, `replace`, `mode`, `word`,
`next`, or "nothing".

```
1.  code == 43 (#):
      reset()                       /* clears _pending_digit and _word_digits */
      if len(modes) > 1:
          _mode_idx = (_mode_idx + 1) % len(modes)
          return ("mode", modes[_mode_idx])
      return ("append", "#")        /* numbers filter only: literal # */

2.  code == 42 (*):
      if mode == "word":
          return ("next", _word_digits) if _word_digits else NOTHING
          /* deliberately does NOT reset(): the digits typed ARE the word */
      reset()
      if mode == "123": return ("append", "*")
      return NOTHING

3.  digit = CODE_TO_DIGIT[code]
    if not a digit code:
        reset()
        return NOTHING              /* nav keys land here and commit the cycle */

4.  if mode == "word":
      if digit not in LETTER_CYCLES:          /* i.e. digit is "0" or "1" */
          _word_digits = ""                   /* word ends; FALL THROUGH */
      else:
          _word_digits += digit
          return ("word", _word_digits)

5.  if mode == "123":
      reset()
      return ("append", digit)

6.  cycle = _cycle_for(digit)
    now = clock()
    if _pending_digit == digit AND (now - _last_press) <= timeout:
        _pending_idx = (_pending_idx + 1) % len(cycle)
        _last_press = now
        return ("replace", cycle[_pending_idx])
    _pending_digit = digit
    _pending_idx = 0
    _last_press = now
    return ("append", cycle[0])
```

Step 4's fall-through is subtle and load-bearing: in predictive mode, pressing `0` or
`1` clears the word sequence and then falls into the ordinary multi-tap path, so the
space/punctuation is produced normally and the caller sees a non-`word` op, which is its
cue to commit the pending word.

#### `_cycle_for(digit)`

```
digit == "0":  ANY → [" ", "0"]           other filters → [" "]
digit == "1":  ANY → PUNCT_CYCLE          other filters → PUNCT_CYCLE_LETTERS
otherwise:     chars = LETTER_CYCLES[digit]
               if mode == "ABC": uppercase every char
               if filter == ANY: append the digit character at the end
```

Worked examples that the tests pin down:

- `any`/`abc`, key 2 four times: `a`, `b`, `c`, `2`, then wraps to `a`.
- `letters`/`abc`, key 2 four times: `a`, `b`, `c`, wraps straight back to `a` — the
  digit is never in the cycle.
- `any`, key 0: `" "` then `"0"`.
- `any`, key 1: `"."` then `","`.
- `numbers`: key 2 always `("append","2")`, never cycles; `*` → `("append","*")`;
  `#` → `("append","#")` and the mode does **not** change.

#### Other engine entry points

| Python | Behaviour |
| --- | --- |
| `mode` (property) | current mode string |
| `modes` (property) | the tuple for this filter, in `#` order |
| `word_digits` (property) | the accumulated predictive sequence |
| `pop_word_digit()` | drops the last digit; returns what remains (possibly `""`), or **`None`** when there was nothing to drop — the caller then treats the key as an ordinary backspace |
| `clear_word()` | `_word_digits = ""` |
| `set_mode_index(i)` | `_mode_idx = i % len(modes)`, then `reset()`, returns the new mode |
| `reset()` | `_pending_digit = None; _word_digits = ""` — note it does **not** clear `_pending_idx` or `_last_press`; that is harmless because `_pending_digit == None` never matches a digit |
| `char_allowed(char, filter)` | `numbers` → `char in "0123456789*#+"`; `letters` → `not char.isdigit()`; `any` → true |

`T9Engine.__init__` raises `ValueError(f"unknown input_filter: {input_filter!r}")` for an
unknown filter. In C: return `ND_ERR_INVAL`.

### 3. `nd_t9_dict` — the predictive dictionary

Ported from `t9_dict.py`.

#### File format

`/NeoDCT/System/core/t9.dict`. Plain ASCII. One word per line, LF-terminated
(**including the last line**). No digits, no counts, no index, no header.

Verified properties of the shipped file:

- 3,022,855 bytes, 315,752 lines
- word lengths 2–12 inclusive
- every line matches `[a-z]+` **except exactly one**: `NeoDCT` at line 163,475
- sorted by `digits_for(word)` ascending; within one digit sequence, by frequency rank

> **Stale comment warning.** `t9_dict.py`'s module docstring says "half a megabyte" and
> "76,000" words, and `mkt9dict.py` defaults to a 512 KiB budget. The shipped file is
> 2.88 MiB / 315,752 words — it was built with a larger `--budget`. Do not size any C
> buffer from the docstring.

`digits_for(word)`: lowercase each character, map through the inverse of
`LETTER_CYCLES`, **return NULL if any character has no letter key** (digits,
apostrophes, spaces all fail). Case-insensitive so `NeoDCT` and `neodct` both key to
`636328`. Tests pin `hello → 43556`, `the → 843`, `NeoDCT → 636328`, and
`it's → None`, `x2 → None`.

#### Lookup constants

```c
#define ND_T9_DICT_PATH     "/NeoDCT/System/core/t9.dict"
#define ND_T9_MAX_SUGGEST   8      /* MAX_SUGGESTIONS */
#define ND_T9_MIN_PREFIX    2      /* shorter prefixes are refused outright */
#define ND_T9_LINE_BACKSCAN 64     /* bytes to reach back for the line start */
```

#### Open / availability

`T9Dictionary(path)` opens the file `"rb"` and `fstat`s it. **An open failure is not an
error** — `available` becomes false and every `suggest()` returns empty, and the phone
falls back to multi-tap. Holds one descriptor and nothing else: no cache, no resident
copy. That property is the whole point and must survive the port.

`shared()` is a lazily-created process-wide singleton. In the C architecture each
process (core, and each app) opens its own — the file is read-only so this is safe, and
one extra fd is cheaper than any IPC.

#### `_line_at(offset)` → `(start, word)`

Reproduce byte for byte:

```
back  = max(0, offset - 64)
chunk = read [back, offset] inclusive   (offset - back + 1 bytes),
        or empty when offset <= back
cut   = last index of '\n' in chunk
start = (cut >= 0) ? back + cut + 1 : back
seek(start); line = readline()
return (start, strip(line) decoded ASCII with invalid bytes ignored)
```

Two quirks to preserve:

- the chunk **includes** the byte at `offset`, so if that byte is itself `'\n'`, `start`
  becomes `offset + 1` — the *next* line;
- `strip()` removes all leading/trailing whitespace, and the decode drops non-ASCII
  bytes rather than failing.

#### `suggest(digits, limit = 8)`

```
if not available or digits empty or len(digits) < 2:  return []
if any char of digits is outside "23456789":          return []

low = 0; high = file_size
while low < high:
    mid = (low + high) / 2                 /* integer division */
    (start, word) = _line_at(mid)
    if start <= low and (high - low) <= 1:
        break                              /* progress guard */
    key = digits_for(word) or ""           /* untypeable line sorts first */
    if key < digits:   low  = start + len(word) + 1
    else:              high = start

results = []
seek(low)
while len(results) < limit:
    raw = readline()
    if raw is empty: break                 /* EOF */
    word = strip(raw)
    if word is empty: continue
    key = digits_for(word)
    if key is NULL: continue
    if not key starts with digits:
        if key > digits: break             /* sorted file: run is over */
        continue
    results.append(word)
return results
```

`key < digits` and `key > digits` are **byte-wise string comparisons** (`strcmp`
semantics on ASCII digit characters), not numeric. `low = start + len(word) + 1` assumes
exactly one `'\n'` per line and no trailing whitespace, which the builder guarantees.

Results come back in file order, so `results[0]` is the likeliest word.

#### `mkt9dict.py` — the builder (stays Python)

It runs on the build host during image assembly. It is **not** ported, but the C
`digits_for()` must agree with it exactly or the binary search walks past words.

- `MIN_LEN = 2`, `MAX_LEN = 12`, `DEFAULT_BUDGET = 512 * 1024`
- `HOUSE_WORDS = ("NeoDCT",)` — get rank `position - len(HOUSE_WORDS)` (so `-1`),
  keep their capitals, are exempt from the length limits and from the budget
- frequency list entries keep list order as their rank
- filler words from the big list rank after everything, sorted by `(len(word), word)`
- **the budget is spent by rank before anything is sorted** — the comment at line 122
  explains why the obvious order (sort first, then truncate) produces a dictionary that
  cannot type a sentence
- cost per word is `len(word) + 1` (the newline)
- final sort key: `(digits_for(word), rank)`
- output: `"\n".join(lines) + "\n"`
- `--add WORD` inserts before the first line whose key is `>= key`, writes to
  `out + ".tmp"` and `os.replace`s — so an interrupted run cannot leave a half-written
  file the phone would binary-search

### 4. `nd_pcf8575` — the I²C expander

Ported from `pcf8575_keypad.py`, class `PCF8575`.

```c
#define ND_I2C_SLAVE      0x0703   /* linux/i2c-dev.h */
#define ND_I2C_BUS_DEFAULT     3
#define ND_I2C_ADDR_DEFAULT 0x20
```

The chip has **no register/command byte**. A plain 2-byte `write()` and a plain 2-byte
`read()` on `/dev/i2c-<bus>` after one `ioctl(fd, I2C_SLAVE, addr)` *are* the correct
raw transactions.

| Operation | Bytes on the wire |
| --- | --- |
| `write16(v)` | `v &= 0xFFFF`, then write `{ v & 0xFF, (v >> 8) & 0xFF }` — **low byte first** |
| `read16()` | read 2 bytes; result is `data[0] | (data[1] << 8)`. A short read raises `OSError("short read from /dev/i2c-N (got N bytes)")` |
| `close()` | `write16(0xFFFF)` first (release every pin, ignoring `OSError`), then `close(fd)`, then `fd = NULL` |

Chip model, quasi-bidirectional, no direction registers:
writing `1` releases the pin to a weak internal pull-up (reads high); writing `0` drives
it hard low.

`__init__` opens `O_RDWR`; if the `I2C_SLAVE` ioctl fails it closes the fd and re-raises.

### 5. `nd_matrix` — the edge-detecting matrix scanner

Ported from `I2CMatrixScanner` and `I2CMatrixKeypadInput`.

```c
#define ND_ROW_PINS_DEFAULT  {0, 1, 2, 3}   /* expander P00-P03 */
#define ND_COL_PINS_DEFAULT  {4, 5, 6, 7}   /* expander P04-P07 */
#define ND_RELEASE_SCANS     3
#define ND_SCAN_SETTLE_US    500            /* 0.0005 s after driving a row */
#define ND_READ_POLL_US      5000           /* 0.005 s between scans in read_key */
```

`RELEASE_SCANS = 3` at the ~5 ms poll cadence gives roughly **15 ms of release
debounce** for mushy membrane contacts.

#### Pin validation

Every pin in `row_pins + col_pins` must be `0 <= pin <= 15` and must not repeat.
Errors: `"expander pin {pin} out of range 0-15"`, `"expander pin {pin} listed twice"`.
Construction also does `chip.write16(0xFFFF)` immediately.

#### `_raw_scan()` → set of pressed `(row_idx, col_idx)`

```
found = {}
for row_idx, row_pin in enumerate(row_pins):
    write16(0xFFFF & ~(1 << row_pin))
    sleep 500 us
    value = read16()
    for col_idx, col_pin in enumerate(col_pins):
        if ((value >> col_pin) & 1) == 0:
            found.add((row_idx, col_idx))
write16(0xFFFF)
return found
```

The whole matrix is scanned every pass rather than stopping at the first hit — that is
what gives key rollover, and games miss direction changes without it.

#### `scan_once()` → one `(row, col)` or nothing

```
current = _raw_scan()
new_presses = [pos for pos in current if pos not in _held]

for pos in current:            _held[pos] = 0
for pos in list(_held):
    if pos not in current:
        _held[pos] += 1
        if _held[pos] >= 3:  delete _held[pos]

if new_presses:
    sort(new_presses)                   /* ascending by (row, col) */
    _pending.extend(new_presses[1:])
    return new_presses[0]
if _pending:  return _pending.pop(0)    /* FIFO */
return None
```

`_held` is a **dict** `(row,col) → consecutive-scans-missing`, and `_pending` is a FIFO
queue of extra simultaneous presses. Both are observable from outside — Koki reads
`scanner._held` to derive true held state (see §11) — so the C struct must expose the
equivalent.

#### `I2CMatrixKeypadInput.read_key(timeout)` → keycode or none

```
timeout = max(0.0, timeout)
deadline = monotonic() + timeout
loop:
    pressed = scan_once()
    if pressed is not None:
        code = matrix_to_code[pressed]
        if code exists:
            _last_unmapped = None
            return code
        if pressed != _last_unmapped:
            _last_unmapped = pressed
            print("[INPUT] I2C matrix key {pressed} has no mapping in {path}")
    if monotonic() >= deadline: return None
    sleep 5 ms
```

Note the loop runs `scan_once()` **at least once** even with `timeout = 0` — the browser
drain path (`Browser/main.py:_drain_input`) depends on `read_key(0)` still consuming a
queued press.

The unmapped-key log is rate-limited to one message per *distinct* position, and is
reset by the next successfully mapped key.

#### The gpiozero variant (`MatrixKeypadInput`, `core/main.py:197`)

Same shape, different transport: `OutputDevice(pin, initial_value=True)` per row,
`Button(pin, pull_up=True)` per column, `row.off()` / `sleep(0.001)` / read
`col.is_pressed` / `row.on()`. **`_held` here is a plain set, not a dict**, and it is
replaced wholesale each scan (`self._held = current`) rather than debounced.

This path only runs when a keymap has `driver != "pcf8575-i2c"` **and** gpiozero
imports **and** `/dev/gpiochip*` exists. On the target hardware none of that is true.
**Recommendation: do not port it.** Record it in `OPEN-QUESTIONS.md` — dropping it
removes the only gpiozero dependency in the whole project.

#### The "single-key limitation"

Worth stating precisely, because it is easy to get wrong:

1. `read_key()` reports **press edges only**. There is no release event on this path at
   all. A consumer that needs "is the key still down" must read the scanner's `_held`
   dict directly.
2. `scan_once()` returns **one** position per call. Simultaneous presses are queued in
   `_pending` and drip out one per call, so a caller polling at 30 Hz sees them over
   several frames.
3. The **I²C scanner does** support rollover (full-matrix scan, dict of held keys). The
   **gpiozero scanner does not** (set, replaced each scan, no debounce).
4. The physical matrix has no diodes, so three keys in an L shape produce a fourth
   phantom press. Nothing in software compensates and nothing should start to.

Koki distinguishes the two backends by type-checking `_held` — see §11.

### 6. `nd_keymap` — `/NeoDCT/User/keymap.json`

Written by the wizard (`i2c_keypad_setup._build_payload`) and by
`consolei2ckeypadbuilder.save()`; read by `core/main.py:_load_matrix_keymap`.

#### Format

```json
{
  "by_code": {},
  "by_matrix": { "0,0": "navikey", "0,1": "clear" },
  "col_pins": [4, 5, 6, 7],
  "driver": "pcf8575-i2c",
  "format": "neodct.keymap.v3.matrix.i2c",
  "generated_at_unix": 1740000000,
  "i2c_addr": 32,
  "i2c_bus": 3,
  "keys": {
    "navikey": { "col": 0, "col_pin": 4, "label": "NaviKey (center)",
                 "row": 0, "row_pin": 0 }
  },
  "output": "/NeoDCT/User/keymap.json",
  "row_pins": [0, 1, 2, 3]
}
```

Serialised with `json.dump(payload, f, indent=2, sort_keys=True)` followed by a literal
`"\n"`, then `flush()` + `fsync()`, written to `path + ".tmp"` and `os.replace`d onto
`path`. `os.makedirs(dirname, exist_ok=True)` first. **Reproduce the atomic-rename and
the fsync** — this file lands on the only writable partition and a torn write bricks
input.

`i2c_addr` is written as an `int` (32 for 0x20), but the loader also accepts a string
`"0x20"` or `"32"`.

#### `_load_matrix_keymap(path)` — refusal paths

Returns `NULL` (and the UI falls through to evdev) in every one of these cases, each
with its exact log line:

| Condition | Log |
| --- | --- |
| file does not exist | *(silent)* |
| JSON parse or read failure | `[INPUT] Keymap read failed ({path}): {exc}` |
| `row_pins` not a list, or `col_pins` not a list, or `keys` not a dict | `[INPUT] Keymap ignored (missing matrix fields): {path}` |
| a pin is not int-convertible | `[INPUT] Keymap ignored (invalid pin list): {exc}` |
| no entry in `keys` resolved to a known name **and** valid row/col | `[INPUT] Keymap ignored (no recognized keys): {path}` |
| `i2c_addr` / `i2c_bus` not parseable | `[INPUT] Keymap ignored (invalid i2c fields): {exc}` |

Per-entry parsing is forgiving: a non-dict entry, an unknown name (not in
`MATRIX_NAME_TO_CODE`), or a non-int `row`/`col` is **skipped silently**, not fatal.

Defaults when absent: `format` → `"unknown"`, `driver` → `"gpiozero-matrix"`,
`i2c_addr` → `0x20`, `i2c_bus` → `3`.

Returned config: `path, format, driver, row_pins[], col_pins[], matrix_to_code{(r,c)→code}, i2c_bus, i2c_addr`.

#### Backend selection (`core/main.py:542`)

```
cfg = load_matrix_keymap("/NeoDCT/User/keymap.json")
if cfg:
    if cfg.driver == "pcf8575-i2c":
        if /dev/i2c-<bus> missing:
            log "[INPUT] Keymap wants {driver}, but {i2c_dev} does not exist."
        else:
            try   I2CMatrixKeypadInput(cfg)
                  log "[INPUT] I2C matrix input active from {path} "
                      "(bus={bus} addr=0x{addr:02X} rows={rows} cols={cols})."
            catch log "[INPUT] I2C matrix init failed; falling back to evdev: {exc}"
    elif gpiozero import failed:
            log "[INPUT] Keymap present, but gpiozero is unavailable: {err}"
    elif no /dev/gpiochip*:
            log "[INPUT] Keymap present, but no /dev/gpiochip* devices were found."
    else:   MatrixKeypadInput(cfg)  /* + its own success / failure logs */
```

**Both backends can coexist with evdev.** The evdev device is opened regardless, and
`read_keypress()` tries the matrix first and falls through to evdev if it produced
nothing.

### 7. `nd_evdev` — reading the kernel input device

From `core/main.py:36`, `:88`, `:1191`.

```c
#define ND_KEYPAD_PATH        "/dev/input/event0"
#define ND_KEYPAD_DEVICE_ENV  "NEODCT_KEYPAD_DEVICE"
```

#### Device discovery (`_discover_keypad_path`), in order

1. `$NEODCT_KEYPAD_DEVICE` if set **and the path exists** → `realpath()` it.
   Log `[INPUT] Using NEODCT_KEYPAD_DEVICE: {selected} ({name})`.
   If set but missing: log `[INPUT] NEODCT_KEYPAD_DEVICE not found: {override}` and
   continue down the list.
2. `sorted(glob("/dev/input/by-path/*-kbd"))`
3. `sorted(glob("/dev/input/by-id/*-kbd"))`
4. `/dev/input/event0` if it exists

Candidates 2–4 are concatenated in that order, `realpath()`d, de-duplicated preserving
order, and the first that exists wins:
`[INPUT] Selected keyboard device: {resolved} ({name})`.

5. Otherwise `sorted(glob("/dev/input/event*"))[0]`:
   `[INPUT] Fallback input device: {fallback} ({name})`
6. Otherwise return `/dev/input/event0`:
   `[INPUT] No input event device found; defaulting to /dev/input/event0`

The `({name})` suffix comes from `_event_device_name(path)`: read
`/sys/class/input/{basename(realpath(path))}/device/name`, stripped, or the literal
string `"unknown"` on any failure.

Open with `O_RDONLY | O_NONBLOCK`. On failure, if the chosen path was not already
`/dev/input/event0`, log `[INPUT] Failed opening {path}: {e}` then
`[INPUT] Falling back to /dev/input/event0` and retry; a second failure logs
`[INPUT] Evdev fallback failed: {e2}` and leaves the fd unset.

Then either `[INPUT] Listening on {path}` or, when there is no matrix backend either,
`[INPUT] WARNING: no active input backend.`

#### Event decoding

```
select([fd], timeout)
if not readable: return None
data = read(fd, 24)
if len(data) == 24: unpack "llHHI"      /* 64-bit host  */
elif len(data) == 16: unpack "IIHHI"    /* 32-bit ARM   */
else: return None
if etype == 1 (EV_KEY) and value == 1 (press):  return code
return None
```

The 24/16 split is `struct input_event` with a 64-bit vs 32-bit `timeval`. On the target
(arm32) it is **16 bytes**. Value 2 (autorepeat) is ignored on this path; only Koki
handles it, and it ignores it too.

Any exception from `select` or `read` returns `None` silently.

#### `read_keypress(timeout = 0.1)` — the composed path

```
_battery_tick(); _modem_tick(); _ring_tick()      /* other subsystems */
if matrix_input:
    key = matrix_input.read_key(timeout)
    if key is not None: return key
    /* falls through to evdev with NO remaining timeout budget */
if keypad_fd is None:
    if matrix_input is None: sleep(max(0, timeout))   /* anti-busy-loop */
    return None
... evdev decode above ...
```

Note the double-wait: when a matrix is present *and* an evdev fd is open, a poll can
block for up to `2 × timeout`. Reproduce it — the frame pacing of every blocking widget
depends on the observed cadence.

`wait_for_key()` is `while True: k = read_keypress(0.1); if k is not None: return k`.

#### Two places that read evdev directly, bypassing the above

- `System/ui/Dialer/call_screen.py:13` hardcodes `KEYPAD_PATH = "/dev/input/event0"` and
  reimplements `_read_keypress` / `_flush_input` with the same 24/16 unpack. It does
  **not** consult the matrix backend, so on keypad-only hardware the in-call screen is
  driven by whatever evdev device exists. Port the duplication as-is or unify and note
  the deviation.
- `System/apps/Koki/engine.py:74` reads the raw fd itself to get press *and* release.

### 8. `nd_uinput` — the virtual keyboard

Ported from `t9_uinput.py`, class `UInputKeyboard`.

```c
#define ND_EV_SYN         0x00
#define ND_EV_KEY         0x01
#define ND_SYN_REPORT     0

#define ND_UI_SET_EVBIT   0x40045564   /* _IOW('U', 100, int) */
#define ND_UI_SET_KEYBIT  0x40045565   /* _IOW('U', 101, int) */
#define ND_UI_DEV_CREATE  0x5501       /* _IO('U', 1)  */
#define ND_UI_DEV_DESTROY 0x5502       /* _IO('U', 2)  */
#define ND_BUS_VIRTUAL    0x06

#define ND_UINPUT_PATH    "/dev/uinput"
#define ND_UINPUT_NAME    "neodct-t9-keypad"
#define ND_UINPUT_SETTLE_US 200000     /* 0.2 s after UI_DEV_CREATE */
```

#### Device creation sequence (exact)

```
fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK)
ioctl(fd, UI_SET_EVBIT, EV_KEY)
for code in needed_keycodes():  ioctl(fd, UI_SET_KEYBIT, code)
write(fd, uinput_user_dev)
ioctl(fd, UI_DEV_CREATE)
sleep 200 ms                       /* let the kernel/console bind the device */
```

Any failure closes the fd, sets it to NULL, and propagates.

`needed_keycodes()` is `sorted(set(_PLAIN.values()) | set(_SHIFTED.values()) | {42} |
set(PASSTHROUGH_CODES))` — **56 codes**, exactly:

```
2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28
30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53
57 103 105 106 108
```

#### `struct uinput_user_dev`

Python packs `"80s4HI64i64i64i64i"` = **1116 bytes** on both 32- and 64-bit
(80 name + 8 input_id + 4 ff_effects_max + 4×64×4 abs arrays; no padding, since 88 is
4-aligned). Field values written:

```
name          = "neodct-t9-keypad", NUL-padded to 80 bytes
id.bustype    = 0x06 (BUS_VIRTUAL)
id.vendor     = 0x1
id.product    = 0x1
id.version    = 1
ff_effects_max = 0
absmax[64] absmin[64] absfuzz[64] absflat[64] = all zero
```

#### `struct input_event`

Python packs `"llHHi"` — 16 bytes on arm32, 24 on x86-64. Timestamp from
`time.time()`: `sec = int(now)`, `usec = int((now - sec) * 1_000_000)`.
Field order: `sec, usec, type, code, value`.

In C, just use the kernel's `struct input_event` and `gettimeofday()`. The important
part is that the field values match.

#### Emission

```
_syn():            emit(EV_SYN, SYN_REPORT, 0)
send_key(code, shift):
    if shift: emit(EV_KEY, 42, 1); syn()
    emit(EV_KEY, code, 1); syn()
    emit(EV_KEY, code, 0); syn()
    if shift: emit(EV_KEY, 42, 0); syn()
```

**A SYN report follows every single key event**, not one per group. The test
`test_type_char_press_release_with_syn` asserts this exactly.

`type_char(ch)` → look up, `send_key`, return true; return **false** and emit nothing
when the char is untypeable. `backspace()` is `send_key(14)`.

`close()`: if we own the device, `ioctl(UI_DEV_DESTROY)` swallowing `OSError`, then
`close(fd)`, then `fd = NULL`. A keyboard constructed with an injected `fd` does **not**
issue `UI_DEV_DESTROY` (`_owns_device = False`) — the tests rely on that, and so should
the C unit tests.

#### `char_to_keypress(char)` → `(keycode, needs_shift)` or nothing

`_PLAIN` (US layout, unshifted):

```
a30 b48 c46 d32 e18 f33 g34 h35 i23 j36 k37 l38 m50 n49
o24 p25 q16 r19 s31 t20 u22 v47 w17 x45 y21 z44
1:2  2:3  3:4  4:5  5:6  6:7  7:8  8:9  9:10  0:11
' ':57  '-':12  '=':13  '[':26  ']':27  ';':39  '\'':40
'`':41  '\\':43  ',':51  '.':52  '/':53  '\t':15  '\n':28
```

`_SHIFTED` (US layout, with shift):

```
!2  @3  #4  $5  %6  ^7  &8  *9  (10  )11
_12 +13 {26 }27 :39 "40 ~41 |43 <51 >52 ?53
```

Resolution order:

1. `char in _PLAIN` → `(_PLAIN[char], false)`
2. `char in _SHIFTED` → `(_SHIFTED[char], true)`
3. `char.isalpha()` and `lower(char) in _PLAIN` → `(_PLAIN[lower], true)`
4. otherwise nothing

So `'Z' → (44, true)`, `'#' → (4, true)` (shift+3), `'*' → (9, true)` (shift+8).

### 9. `nd_t9_bridge` — shell and browser bridges

#### `T9ShellBridge`

Constructed with `(read_key_callable, keyboard, engine = T9Engine(), poll_timeout = 0.05)`.

```
handle_code(code):
    if code in PASSTHROUGH_CODES (28,14,103,105,106,108):
        engine.reset()
        keyboard.send_key(code)
        return
    op = engine.press(code)
    if op is None: return
    kind, value = op
    if kind == "append":  keyboard.type_char(value)
    if kind == "replace": keyboard.backspace(); keyboard.type_char(value)
    /* "mode" produces nothing: the shell prompt has no indicator */
    /* "word"/"next" cannot occur — the bridge's engine never reaches predictive
       from the # cycle in practice, but if it does the ops are simply ignored */
```

Thread lifecycle: `start()` clears the stop event and spawns a **daemon** thread named
`"t9-shell-bridge"`. The loop is:

```
while not stopped:
    try: code = read_key(0.05)
    except: sleep(0.1); continue
    if code is None: continue
    try: handle_code(code)
    except: pass          /* a bad keypress must never kill the bridge */
```

`stop()` sets the event, `join(timeout = 2.0)`, clears the thread handle, then
`keyboard.close()` swallowing exceptions.

In C this is one `pthread`. **Note the fork/exec rule in CODING-STANDARDS §1.1**: the
shell bridge exists precisely because the UI has forked a child (the shell / netsurf)
and is blocked in `waitpid()`. Start the bridge thread *after* the fork, or make
absolutely sure no fork happens while it holds a lock.

`start_shell_bridge(ui, keyboard_factory = UInputKeyboard)`:

```
matrix = ui.matrix_input
if matrix is None: return None        /* QEMU/dev: a real keyboard already works */
try: keyboard = keyboard_factory()
except exc: print(f"[T9] uinput keyboard unavailable: {exc}"); return None
bridge = T9ShellBridge(matrix.read_key, keyboard); bridge.start(); return bridge
```

The caller must `stop()` it when the child exits.

#### `T9BrowserBridge`

```c
#define ND_CURSOR_MODE "nav"

/* keypad code -> Linux arrow */
3 (keypad 2) → KEY_UP    103
5 (keypad 4) → KEY_LEFT  105
6 (keypad 5) → KEY_ENTER  28    /* follow the highlighted link */
7 (keypad 6) → KEY_RIGHT 106
9 (keypad 8) → KEY_DOWN  108
```

The enrolment collects Up and Down but **no Left or Right at all**, so netsurf can never
see a horizontal arrow unless the number pad stands in for one. That is why this table
exists.

State: `_pos` (0 = cursor, 1..n = `_modes[pos-1]`), and `_modes` is the engine's mode
tuple **with `MODE_WORD` filtered out** — predictive answers a keypress with a candidate
list and there is no candidate UI on the other side of a uinput keyboard.

So for the default `any` filter the cycle is: `nav → abc → ABC → 123 → nav`.

```
cycle_mode():
    engine.reset()
    _pos = (_pos + 1) % (len(_modes) + 1)
    if _pos: engine.set_mode_index(engine.modes.index(_modes[_pos - 1]))
    return mode

handle_code(code):
    if code == 43 (#): cycle_mode(); return
    if cursor_mode:
        arrow = BROWSER_NAV_CODES[code]
        if arrow:  keyboard.send_key(arrow)
        elif code in PASSTHROUGH_CODES:  keyboard.send_key(code)
        /* every other key is inert: typing a letter by accident while
           scrolling is worse than the press doing nothing */
        return
    super().handle_code(code)
```

`start_browser_bridge(ui, keyboard_factory)` is identical to the shell variant with
`T9BrowserBridge` substituted, same `[T9] uinput keyboard unavailable: {exc}` message.

### 10. `nd_keypad_setup` — the first-boot wizard

Ported from `i2c_keypad_setup.py`. **This is drawn to the framebuffer before the UI
framework exists, so it owns its own drawing.** Every coordinate below is exact.

```c
#define ND_KEYMAP_PATH        "/NeoDCT/User/keymap.json"
#define ND_SETUP_BUS_ENV      "NEODCT_KEYPAD_SETUP_BUS"
#define ND_DEFAULT_SETUP_BUS  3
#define ND_PROBE_ADDR_FIRST   0x20
#define ND_PROBE_ADDR_LAST    0x27       /* range(0x20, 0x28) */
#define ND_FIRST_KEY_TIMEOUT  120.0      /* seconds, first press only */
#define ND_KEY_TIMEOUT        60.0       /* seconds, every later press */
#define ND_SETUP_RELEASE_SCANS 3
#define ND_SETUP_FONT "/NeoDCT/System/ui/resources/fonts/font.ttf"
#define ND_SETUP_UI_W 240
#define ND_SETUP_UI_H 175
```

#### Enrolment order (`KEY_TARGETS`) — 16 keys, this order exactly

| # | name | on-screen label |
| ---: | --- | --- |
| 1 | `navikey` | `NaviKey (center)` |
| 2 | `clear` | `C (clear/back)` |
| 3 | `up` | `Up` |
| 4 | `down` | `Down` |
| 5 | `num_1` | `1` |
| 6 | `num_2` | `2` |
| 7 | `num_3` | `3` |
| 8 | `num_4` | `4` |
| 9 | `num_5` | `5` |
| 10 | `num_6` | `6` |
| 11 | `num_7` | `7` |
| 12 | `num_8` | `8` |
| 13 | `num_9` | `9` |
| 14 | `num_0` | `0` |
| 15 | `star` | `*` |
| 16 | `hash` | `#` |

No `left`, `right`, `menu`, `enter` or `back` are enrolled. The console builder
(`consolei2ckeypadbuilder.py`) uses the same list with one different label:
`NaviKey (center/enter)`.

#### Fonts

```
font_big   = truetype(font.ttf, 26)
font       = truetype(font.ttf, 18)
font_small = truetype(font.ttf, 14)
```

If **any** of the three fails to load, all three become `ImageFont.load_default()`.
(Note: `font_big` is loaded first, so a failure at size 26 discards the other two even
if they would have loaded.)

#### Drawing primitives

Canvas: `Image.new("RGB", (240, 175), "black")`, reused between frames — the code never
allocates a new canvas.

```
_text_size(text, font) = (bbox[2]-bbox[0], bbox[3]-bbox[1])
                          where bbox = draw.textbbox((0,0), text, font=font)
_center(text, font, y, fill="white"):
    w = _text_size(...)[0]
    draw.text(((240 - w) // 2, y), text, font=font, fill=fill)
```

Colours: `"black"` = (0,0,0), `"white"` = (255,255,255), `"gray"` = (128,128,128).

`message(title, lines=(), footer=None)`:

```
draw.rectangle((0, 0, 240, 175), fill="black")     /* inclusive coords, clipped */
_center(title, font(18), y=18)
y = 58
for line in lines:  _center(line, font_small(14), y);  y += 20
if footer: _center(footer, font_small, y=151, fill="gray")     /* 175 - 24 */
fb.update(canvas)
```

`prompt(label, index, total, note=None)`:

```
draw.rectangle((0, 0, 240, 175), fill="black")
_center("Keypad setup", font_small, y=6, fill="gray")

counter = "{index+1}/{total}"
w = _text_size(counter, font_small)[0]
draw.text((240 - 8 - w, 6), counter, font=font_small, fill="white")

_center("Press:", font(18), y=46)
_center(label, font_big(26), y=76)

bar_y = 133                                        /* 175 - 42 */
draw.rectangle((16, 133, 224, 141), outline="white")   /* 240-16 = 224, +8 */
if index > 0:
    fill_w = int(206 * (index / total))             /* 240 - 34 = 206 */
    draw.rectangle((17, 134, 17 + fill_w, 140), fill="white")

if note: _center(note, font_small, y=151)           /* 175 - 24 */
fb.update(canvas)
```

#### `PairScanner` — the 16-pin discovery scan

Unlike the runtime scanner, this makes **no row/col assumption**. It drives each of the
16 pins low in turn and records every *unordered* pin pair that reads low together:

```
scan_pairs():
    seen = {}
    for drive in 0..15:
        write16(0xFFFF & ~(1 << drive))
        sleep 500 us
        value = read16()
        for bit in 0..15:
            if bit != drive and ((value >> bit) & 1) == 0:
                seen.add((min(drive, bit), max(drive, bit)))
    write16(0xFFFF)
    return seen
```

Note this is 16 write+read pairs per scan, not 4 — the discovery scan is four times as
expensive as the runtime scan, which is fine because it only runs during enrolment.

```
wait_release(max_seconds = 10.0):
    deadline = monotonic() + 10.0
    empties = 0
    while monotonic() < deadline:
        if scan_pairs() is empty:
            empties += 1
            if empties >= 3: return
        else:
            empties = 0
        sleep 10 ms

wait_new_pair(timeout) -> pair or None:
    deadline = monotonic() + timeout
    while monotonic() < deadline:
        pairs = scan_pairs()
        if len(pairs) == 1: return the one pair      /* exactly one, not >= 1 */
        sleep 10 ms
    return None
```

`wait_new_pair` requires **exactly one** pressed pair — two keys held at once are
ignored until one is released.

#### `_bipartition(pairs)` — deciding which pins are rows

2-colour the connection graph. A key joins one pin from each side, so pins that are ever
joined must be on opposite sides.

```
adj = undirected adjacency from pairs
color = {}
conflicts = []
for start in sorted(adj):                  /* ascending pin number */
    if start already coloured: continue
    color[start] = 0
    queue = [start]
    while queue:
        node = queue.pop()                 /* pop from the END: this is DFS */
        for nb in adj[node]:               /* iteration order of a Python set */
            if nb not coloured:
                color[nb] = 1 - color[node]
                queue.append(nb)
            elif color[nb] == color[node]:
                conflicts.append((node, nb))
side_a = sorted(pins with colour 0)
side_b = sorted(pins with colour 1)
return side_a, side_b, conflicts
```

`row_pins = side_a`, `col_pins = side_b`. Which physical side ends up as "rows" depends
on the smallest-numbered pin in each connected component — that is deterministic given
the pair set, and it does not matter electrically because a plain matrix is symmetric.

> **Portability note for the C author.** The Python iterates a `set` of neighbours,
> whose order is unspecified. The *colouring* is unaffected (a bipartition of a
> connected component is unique up to swapping the two colours, and the colour of the
> component's smallest pin is pinned to 0). Only the **order of entries in
> `conflicts`** can differ, and only the first conflict is ever reported. Use a sorted
> neighbour list in C and note the deviation — it is a message-text difference on a
> path that already means "your keypad is miswired".

#### `_build_payload(pair_by_name, bus, addr)`

```
all_pairs = the set of every recorded pair
side_a, side_b, conflicts = _bipartition(all_pairs)
if conflicts:  return (None, "P{a}/P{b} conflict")    /* conflicts[0] */

row_pins, col_pins = side_a, side_b
for each (name, label) in KEY_TARGETS that was captured, in KEY_TARGETS order:
    a, b = pair_by_name[name]
    if a in row_pins and b in col_pins:  row_pin, col_pin = a, b
    elif b in row_pins and a in col_pins: row_pin, col_pin = b, a
    else: return (None, "key '{name}' does not fit the matrix split")
    keys[name] = { label, row: row_pins.index(row_pin),
                          col: col_pins.index(col_pin),
                          row_pin, col_pin }
```

Then the payload described in §6, with `generated_at_unix = int(time.time())` and
`output = "/NeoDCT/User/keymap.json"`.

#### `run_wizard(fb, chip, addr, bus, restart=True)`

```
message("Keypad setup",
        ("Keypad found on bus {bus}",
         "(PCF8575 at 0x{addr:02X})",
         "Press each key as asked."))
sleep 2.0

for index, (name, label) in enumerate(KEY_TARGETS):
    timeout = 120.0 if index == 0 else 60.0
    loop:
        prompt(label, index, 16, note)
        pair = scanner.wait_new_pair(timeout)
        if pair is None:
            message("Setup aborted",
                    ("No key was pressed.", "Starting without a keymap."))
            sleep 2.5
            return false
        if pair already used:
            note = "Already used by '{label_of_the_owner}'"
            scanner.wait_release()
            continue          /* redraws the prompt with the note */
        record it;  note = None;  scanner.wait_release();  break

payload, err = _build_payload(...)
if payload is None:
    message("Setup failed", (err, "Starting without a keymap."))
    sleep 3.0;  return false

try _save_keymap(payload)
except exc:
    message("Setup failed", ("Could not save: {exc}",))
    sleep 3.0;  return false

message("Keymap saved!", ("{n} keys mapped.", "Restarting UI..."))
sleep 1.5
if restart:
    chip.close()
    os.execv(sys.executable, [sys.executable] + sys.argv)   /* never returns */
return true
```

The `note` persists across the redraw of the *same* prompt (duplicate key) but is
cleared once a key is accepted.

**The restart.** `os.execv` re-executes the interpreter with the same argv. In C the
equivalent is `execv("/proc/self/exe", saved_argv)` — but the core process has threads
by then, and `execve` from a threaded process is legal and does the right thing (all
other threads are destroyed). Still, do the `chip.close()` first so the expander is left
with every pin released.

#### `maybe_run_first_time_setup(fb, restart=True)` — the boot gate

Called from `core/main.py:run()` **before `NeoDCT_UI` is constructed**, wrapped in a
`try/except` that prints `[SETUP] First-time keypad setup failed; continuing boot.` plus
a traceback and carries on.

```
if exists("/NeoDCT/User/keymap.json"):
    print("[SETUP] Keymap already present (/NeoDCT/User/keymap.json); skipping setup.")
    return false

bus = int(getenv("NEODCT_KEYPAD_SETUP_BUS", "3"))
dev = "/dev/i2c-{bus}"
is_hw = exists("/dev/ttyFIQ0")        /* Rockchip FIQ console: real HW only */

if not exists(dev) and not is_hw:
    return false                       /* QEMU / dev box: silent */

if not exists(dev):
    message("Keypad setup", ("Waiting for {dev}...",))
    poll every 0.25 s for up to 8.0 s
    if still absent:
        print("[SETUP] {dev} never appeared; no keymap and no bus. "
              "Set NEODCT_KEYPAD_SETUP_BUS if the keypad is on another bus.")
        message("No keypad bus", ("{dev} does not exist.",
                                  "Starting without a keymap."))
        sleep 3.0;  return false

chip, addr = _probe_chip(bus)
if chip is None:
    print("[SETUP] No PCF8575 answered on {dev} (tried 0x20-0x27).")
    message("No keypad found", ("Nothing answered on {dev}",
                                "(addresses 0x20-0x27).",
                                "Starting without a keymap."))
    sleep 3.0;  return false

print("[SETUP] No keymap; PCF8575 found at 0x{addr:02X} on bus {bus}. "
      "Starting first-time keypad setup.")
try:    return run_wizard(fb, chip, addr, bus, restart)
finally: chip.close() swallowing exceptions
```

`_probe_chip(bus)` walks `0x20 .. 0x27` and returns the **first** address where
`PCF8575(bus, addr)` constructs, `write16(0xFFFF)` succeeds and `read16()` succeeds.
Because the PCF8575 has no registers, *any* device that ACKs at that address will
answer, so probe order is the tiebreaker.

> **Latent Python bug, do not reproduce.** `_probe_chip`'s `except OSError` branch calls
> `chip.close()` where `chip` may be unbound (constructor raised) — a `NameError`
> swallowed by an inner `except Exception: pass`, or, worse, a close of the *previous*
> iteration's chip. In C, close only what you actually opened. Behaviour is unchanged
> because `PCF8575.close()` is idempotent.

### 11. `neodctDisplay.c` — the ST7789 daemon (already C)

**Assessment: reuse as-is as a standalone binary. Do not fold it into the core.**

Reasons:

1. It is already C, already builds, and is the one piece of hardware-facing code the
   project has proven on this exact board and panel. Rewriting it buys nothing.
2. It must run **before** the UI so the framebuffer is already 240×175@32bpp when the UI
   opens it (`S90display` starts at init level 90; `core/main.py:341` prints a warning
   if it finds a 16bpp fb).
3. It must also run **inside the initramfs**, where there is no core process at all —
   `mkinitramfs.py:306` copies the same prebuilt binary to `bin/neodct_displayd` and
   `ndsys-recovery.sh:43` starts it to paint the recovery screen. Folding it into the
   core would leave recovery blind on hardware.
4. It is ~120 KB of a 53 MB budget, single-threaded, and idles at ~0 CPU thanks to frame
   skipping. There is no memory case for merging it.
5. Two processes must **never** drive the same SPI bus at once — `ndsys-recovery.sh`
   `panel_stop()`s before `switch_root` for exactly this reason. Keeping it a separate
   process with an obvious PID is what makes that manageable.

What the port *does* need to do:

- keep it building under the project's new warning set (`-Wall -Wextra -Werror
  -Wconversion` will flag the `unsigned`/`int` mixing in `render_dirty()` and
  `convert_rect()`); fix the warnings, **do not change the arithmetic**;
- keep the binary at `/NeoDCT/System/hw/neodct_displayd` — `S90display`,
  `mkinitramfs.py:253` and `/etc/profile.d/neodct-path.sh` all name that path;
- keep the CLI flags — `S90display` passes `--speed 16`.

#### Constants that define the visible geometry

```c
#define DC_PIN     57          /* GPIO1_D1, physical pin 13 */
#define RESET_PIN  56          /* GPIO1_D0, physical pin 12 */
#define PANEL_W    240
#define PANEL_H    240
#define OFFSET_X   0
#define OFFSET_Y   0           /* try 80 if the image is shifted (240x240 quirk) */
#define FB_W       240
#define FB_H       175
#define DEFAULT_Y_OFFSET (PANEL_H - FB_H)   /* 65 */
#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_BITS   8
#define SPI_MODE   0
#define FB_DEVICE  "/dev/fb0"
#define DEFAULT_SPI_SPEED 20000000
#define DEFAULT_FPS       30
#define STATS_INTERVAL_MS 5000.0
```

Wiring (also in `docs/HARDWARE_NOTES.md`): CS = pin 6 (SPI0_CS0), CLK = pin 7, MOSI =
pin 8 (**not** pin 9), RST = pin 12 (gpio56), DC = pin 13 (gpio57), BL = pin 11
(gpio53).

#### Panel init sequence (proven on this exact panel)

```
RESET: gpio 1, 100 ms; gpio 0, 100 ms; gpio 1, 120 ms
0x01 SWRESET,           150 ms
0x11 SLPOUT,            150 ms
0x3A COLMOD data 0x55,   10 ms      /* RGB565 */
0x36 MADCTL data 0x00,   10 ms
0x21 INVON,              10 ms      /* required on this IPS panel */
0x13 NORON,              10 ms
0x29 DISPON,            150 ms
```

`set_window(x0,y0,x1,y1)`: add `OFFSET_X`/`OFFSET_Y`, then `0x2A CASET` with 4 bytes
big-endian `{x0>>8, x0&0xFF, x1>>8, x1&0xFF}`, `0x2B RASET` likewise, then `0x2C RAMWR`.

Pixel packing is **big-endian RGB565**: `((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)`,
emitted high byte first.

DC low = command, DC high = data. The DC value fd is cached open; RESET uses the slow
open/write/close path.

#### Framebuffer mode forcing (`force_mode`)

Ask for `xres = 240, yres = 175, xres_virtual = 240, yres_virtual = 175,
bits_per_pixel = 32`. If `FBIOPUT_VSCREENINFO` fails, **or** the readback still is not
32, retry the same geometry at 16 bpp. Accept only 16 or 32; anything else is a fatal
error `"expected 16 or 32 bpp framebuffer (got %u)"`.

32 bpp is preferred because Pillow's fast `"BGRA"` rawmode packer exists and its
`"BGR;16"` packer was removed in Pillow 11. **After the C rewrite that reason
evaporates** — the in-house rasterizer can emit either. Keep 32 bpp anyway: the daemon's
dirty-rect diff is done on the fb bytes, and 4 bytes/px makes the diff and the copy
trivially aligned. Flag as an open question if the memory saving of a 16 bpp fb
(240×175×2 = 84,000 vs 168,000 bytes, plus the same again for `prev_fb`) is wanted.

#### Dirty-rect algorithm (`render_dirty`)

```
max_h  = PANEL_H - opt_yoff                     /* 175 with the default */
copy_w = min(vinfo.xres, PANEL_W)
copy_h = min(vinfo.yres, max_h)
row_bytes = copy_w * fb_bytespp

pass 1: for each row y in [0, copy_h):
            memcmp(fb + y*line_length, prev + y*line_length, row_bytes)
        track ymin (first differing) and ymax (last differing)
        if ymin > ymax: return 0                /* identical frame: skip */

pass 2: for y in [ymin, ymax]:
            skip clean rows
            i = first differing byte from the left
            j = last differing byte from the right (while j > i)
            bmin = min(bmin, i);  bmax = max(bmax, j)
        xmin = bmin / fb_bytespp;  xmax = bmax / fb_bytespp

copy the dirty rows [ymin, ymax] into prev_fb
convert_rect(xmin, ymin, xmax, ymax)
set_window(xmin, ymin + opt_yoff, xmax, ymax + opt_yoff)
write_data(out_buf, n)
```

`prev_fb` is `malloc(fb_size)` and `memset` to **0xA5** so the first frame is always
sent in full.

`convert_rect` handles both fb depths: 32 bpp reads bytes `B,G,R,X` and packs 565;
16 bpp byte-swaps the native little-endian 565. `--swap-rb` swaps R/B in both paths
(16 bpp does it by re-shuffling the 565 fields, which is not exactly the same operation
as the 32 bpp channel swap — a pre-existing asymmetry; port as-is).

`render_full` sends the whole `copy_w × copy_h` band unconditionally and updates the
whole of `prev_fb` — note it copies `copy_w * fb_bytespp` bytes per row here rather than
`row_bytes` (they are the same value; two names for it in the original).

#### Buffers

`out_buf` is `malloc(PANEL_W * PANEL_H * 2)` = **115,200 bytes**, allocated once at
startup and reused for every rect and for `fill_color`.
`prev_fb` is `malloc(finfo.line_length * vinfo.yres)` = **168,000 bytes** at
240×175@32bpp. Total steady-state heap ≈ **283 KB**. That is the daemon's whole
footprint and it is fine.

#### Main loop and CLI

```
--test / -t     R/G/B/W/K full-screen fills, 800 ms each, then exit 0
--once          one full frame then exit
--full          disable diffing (v1 behaviour)
--swap-rb       swap R/B channels
--stats         print a stats line every 5 s
--speed N       SPI Hz; values < 1000 are multiplied by 1,000,000
--yoff N        clamped to [0, PANEL_H - 1]
--fps N         clamped to >= 1
anything else   "unknown arg: %s" and exit 2
```

Startup order: parse args → install SIGINT/SIGTERM handlers (set `quit_flag`) → print
the banner → allocate `out_buf` → `setup_gpio()` → `init_spi()` → `reset_display()` →
`panel_init()` → `fill_color(0,0,0)` (so rows outside the band are defined black —
v1 re-blanked them every frame) → `--test` exits here → `init_framebuffer()` → loop.

Loop pacing: `frame_ms = 1000.0 / opt_fps`; after each pass, `usleep((frame_ms -
elapsed) * 1000)` when it ran fast.

SPI chunking: read `/sys/module/spidev/parameters/bufsiz`; use it if `>= 4096`,
otherwise 4096. Each chunk is one `SPI_IOC_MESSAGE(1)` with `speed_hz = opt_speed`,
`bits_per_word = 8`, `delay_usecs = 0`. Boot with `spidev.bufsiz=65536` to cut the ioctl
count 16×.

GPIO is sysfs (`/sys/class/gpio/export`, `.../gpioN/direction`, `.../gpioN/value`) with
a 10 ms sleep after export. `gpio_export` is a no-op if `/sys/class/gpio/gpioN` already
exists.

#### The framebuffer contract with the UI

The UI writes a 240×175 image into `/dev/fb0` and the daemon mirrors it. On the fast
path (`core/main.py:392`) the source is exactly the fb size, so `dst_x = dst_y = 0` and
the whole band is one contiguous `mmap` write of `240 × 175 × 4 = 168,000` bytes in
**BGRA** order. That is the format the C rasterizer must produce, and it is the same
format `mkinitramfs.bmp_to_xrgb8888()` produces for `bootlogo.raw` / `splash.raw`
(bytes B, G, R, 0, rows top-down).

### 12. `nd_backlight`

Ported from `backlight.py`. Three tiers, best first.

```c
#define ND_BL_GPIO_PIN        53          /* GPIO1_C5, header pin 11 */
#define ND_BL_GPIO_ROOT       "/sys/class/gpio"
#define ND_BL_BACKLIGHT_ROOT  "/sys/class/backlight"
#define ND_BL_ACTIVE_LOW      0           /* overridable; getting it backwards
                                             darkens the screen exactly when
                                             somebody starts using the phone */
#define ND_BL_MIN_ON_PERCENT  5
```

`mode()`: PWM if `/sys/class/backlight/*/brightness` exists (first match in
`sorted(glob)`); else GPIO if `/sys/class/gpio` is a directory and the pin exports;
else `none`. `available()` is `mode() != none`.

`set_percent(p)`:

```
p = clamp(int(p), 0, 100)
if 0 < p < 5: p = 5          /* "on but very dim" must not read as a broken screen */
PWM:  top = int(read(max_brightness) or 255)
      write(brightness, round(top * p / 100.0))
GPIO: lit = p > 0
      write(value, "0" if lit == ACTIVE_LOW else "1")
none: return false
```

Python's `round()` is **banker's rounding** (round-half-to-even). `round(255 * 50 /
100.0)` = `round(127.5)` = **128**, not 127. Reproduce it or the brightness slider is
off by one at half-way points. Every write returns false on `OSError` rather than
raising.

`get_percent()`: PWM → `int(round(now * 100.0 / max(1, top)))`; GPIO → 100/0 from the
pin value honouring `ACTIVE_LOW`; else `None`. `off()` = `set_percent(0)`,
`on(p=100)` = `set_percent(p)`.

`_export_gpio()`: if `/sys/class/gpio/gpioN` is not a directory, write `N` to
`/sys/class/gpio/export` (EBUSY is fine — somebody exported it already); then if the
direction is not `"out"`, write `"out"`; return whether `value` exists.

### 13. `neodct-sdcard` — no port needed

274 lines of busybox `sh`. `AGENTS.md` already mandates busybox ash for shell scripts,
and the udev rule
(`/etc/udev/rules.d/60-neodct-sdcard.rules`) invokes it by absolute path. It publishes
`state=mounted|unmountable|share|absent  device=  fstype=  label=` to
`/run/neodct/sdcard.prop`, which `System/core/Storage` reads. **Keep verbatim.**

The only thing the C port must honour is the state-file format and the mount point
`/NeoDCT/User/sdcard` (overridable via `NEODCT_SDCARD_MOUNT`). Card filesystem order is
`vfat exfat ext4 ext3 ext2`; FAT variants mount with `rw,noatime,utf8,flush`, others
with `rw,noatime`. Format creates `mkfs.vfat -F 32 -n NEODCT` and the folders
`wallpapers tones backup_db music update`.

### 14. Consumers whose behaviour depends on this subsystem

Not ported here, but the interfaces below are load-bearing contracts.

**UI framework text widgets** (`System/ui/framework.py`) drive the engine and the
dictionary. The op → widget-action mapping in `TextInput.handle_key`:

```
key 28 or 96 → "confirm"
key 14       → _predict_backspace() ? "backspace"
               : commit word, t9.reset(), delete last char → "backspace",
                 or "cancel" when the text is already empty
otherwise, when a matrix keypad is present:
    op = t9.press(key)
    op is None                     → None
    _predict_key(op) not None      → that ("typed")
    kind "append"  → text += value → "typed"
    kind "replace" → text = text[:-1] + value → "typed"
    kind "mode"                    → "mode"
otherwise (dev QWERTY keyboard):   DEV_KEYMAP lookup + char_allowed()
```

`_t9_active(ui)` is literally `getattr(ui, "matrix_input", None) is not None` — T9
multi-tap runs **only** on the real i²c keypad; a QEMU keyboard uses the QWERTY
`DEV_KEYMAP` path and shows no mode indicator at all.

> **In C this cannot be asked the Python's way from inside an app.** An app's
> `ui->input` is the pipe the core hands it, which has no matrix by construction, so
> `nd_input_has_matrix()` is the wrong source for `ui->has_matrix_keypad` there — it
> answers `false` on the phone and takes all of T9 down with it. The core sets
> **`NEODCT_KEYPAD_MATRIX=1`** (`nd_app.h`) and `nd_ui_init_app()` reads that.
> `NEODCT_T9` overrides the result in either direction, which is the only way to
> exercise T9 over a QWERTY keyboard. See OPEN-QUESTIONS.md BR-3.

**Koki** (`System/apps/Koki/engine.py:39`) has its own keycode map and needs *held*
state, which `read_key()` cannot give it:

```
105 left  106 right  103 up  108 down  44 z  45 x  28 enter  14 back
5 left    7 right      3 up    9 down          /* keypad 4/6/2/8 */
6 z (jump)  11 x (action)  42 z (star)  43 x (hash)
```

It distinguishes backends by `isinstance(scanner._held, dict)`: dict → rollover, derive
the held set from the scanner's own debounced state; otherwise → single-key, a new press
replaces the previous held key. The C API must expose the equivalent of "the set of
positions currently held, debounced" or Koki cannot be ported faithfully.

**Browser** (`System/apps/Browser/main.py:192`) drains both queues before returning to
the UI: `read(fd, 4096)` in a `select` loop for evdev, and up to **64** calls of
`matrix.read_key(0)` for the scanner's `_pending`.

**Dialer** (`System/ui/Dialer/call_screen.py`) reads `/dev/input/event0` directly.

---

## Public interface (the functions other parts call)

Suggested C signatures. Names follow `nd_<module>_<verb>` per CODING-STANDARDS §2.

### `nd_t9_engine.h`

```c
typedef enum { ND_T9_FILTER_ANY, ND_T9_FILTER_LETTERS, ND_T9_FILTER_NUMBERS } nd_t9_filter;
typedef enum { ND_T9_MODE_WORD, ND_T9_MODE_ABC, ND_T9_MODE_UPPER, ND_T9_MODE_123 } nd_t9_mode;
typedef enum { ND_T9_OP_NONE, ND_T9_OP_APPEND, ND_T9_OP_REPLACE,
               ND_T9_OP_MODE, ND_T9_OP_WORD,   ND_T9_OP_NEXT } nd_t9_opkind;

typedef struct {
    nd_t9_opkind kind;
    char         ch;            /* APPEND / REPLACE */
    nd_t9_mode   mode;          /* MODE */
    const char  *digits;        /* WORD / NEXT -- points into the engine */
} nd_t9_op;

typedef double (*nd_clock_fn)(void *ctx);   /* monotonic seconds; injectable */

typedef struct nd_t9_engine nd_t9_engine;   /* opaque; ~64 bytes, embeddable */

nd_err       nd_t9_engine_init(nd_t9_engine *e, nd_t9_filter f,
                               double timeout_s, nd_clock_fn clk, void *clk_ctx);
nd_t9_op     nd_t9_engine_press(nd_t9_engine *e, int32_t code);
nd_t9_mode   nd_t9_engine_mode(const nd_t9_engine *e);
const nd_t9_mode *nd_t9_engine_modes(const nd_t9_engine *e, size_t *count);
const char  *nd_t9_engine_word_digits(const nd_t9_engine *e);
/* returns the remaining digits, or NULL when there was nothing to drop */
const char  *nd_t9_engine_pop_word_digit(nd_t9_engine *e);
void         nd_t9_engine_clear_word(nd_t9_engine *e);
nd_t9_mode   nd_t9_engine_set_mode_index(nd_t9_engine *e, size_t index);
void         nd_t9_engine_reset(nd_t9_engine *e);
bool         nd_t9_char_allowed(char c, nd_t9_filter f);
const char  *nd_t9_mode_label(nd_t9_mode m);   /* "word"/"abc"/"ABC"/"123" */
```

`_word_digits` is bounded by the longest dictionary key (12), but nothing enforces that
in the Python — a user can hold down 2 and accumulate an arbitrarily long sequence.
**Cap the C buffer at 32 digits and stop appending past it**, and record the deviation:
`suggest()` returns nothing for anything longer than 12 anyway.

### `nd_t9_dict.h`

```c
typedef struct nd_t9_dict nd_t9_dict;

nd_t9_dict *nd_t9_dict_open(const char *path);   /* NULL is not an error */
void        nd_t9_dict_close(nd_t9_dict *d);
bool        nd_t9_dict_available(const nd_t9_dict *d);
nd_t9_dict *nd_t9_dict_shared(void);             /* process-wide, lazy */

/* digits_for: writes at most out_sz bytes; false when the word is untypeable */
bool nd_t9_digits_for(const char *word, char *out, size_t out_sz);

/* Fills caller-owned storage. Returns the count written (0..limit).
   Words are at most 12 bytes + NUL; ND_T9_WORD_MAX = 16 gives slack. */
size_t nd_t9_dict_suggest(nd_t9_dict *d, const char *digits,
                          char out[][ND_T9_WORD_MAX], size_t limit);
```

No allocation on the suggest path — the caller supplies the array. `MAX_SUGGESTIONS` is
8, so a `char[8][16]` = 128 bytes on the stack is the whole cost.

### `nd_pcf8575.h` / `nd_matrix.h`

```c
typedef struct { int fd; int bus; int addr; char dev_path[24]; } nd_pcf8575;

nd_err nd_pcf8575_open(nd_pcf8575 *c, int bus, int addr);
nd_err nd_pcf8575_write16(nd_pcf8575 *c, uint16_t value);
nd_err nd_pcf8575_read16(nd_pcf8575 *c, uint16_t *out);
void   nd_pcf8575_close(nd_pcf8575 *c);          /* writes 0xFFFF first */

typedef struct { uint8_t row, col; } nd_matrix_pos;

typedef struct nd_matrix_scanner nd_matrix_scanner;

nd_err nd_matrix_scanner_init(nd_matrix_scanner *s,
                              const uint8_t *row_pins, size_t n_rows,
                              const uint8_t *col_pins, size_t n_cols,
                              int bus, int addr);
/* one full pass; true when a position was produced */
bool   nd_matrix_scan_once(nd_matrix_scanner *s, nd_matrix_pos *out);
/* debounced held set, for Koki. Returns count; fills up to max entries. */
size_t nd_matrix_held(const nd_matrix_scanner *s, nd_matrix_pos *out, size_t max);
void   nd_matrix_scanner_close(nd_matrix_scanner *s);
```

`_held` and `_pending` can both be fixed-size arrays: at most `n_rows * n_cols <= 64`
positions exist, so 64 entries of `{pos, missing_count}` is 192 bytes. **No hash map,
no allocation.**

```c
typedef struct nd_matrix_input nd_matrix_input;

nd_err  nd_matrix_input_open(nd_matrix_input *in, const nd_keymap *cfg);
int32_t nd_matrix_input_read_key(nd_matrix_input *in, double timeout_s); /* -1 = none */
void    nd_matrix_input_close(nd_matrix_input *in);
```

### `nd_keymap.h`

```c
typedef struct {
    char     path[128];
    char     format[48];
    char     driver[32];
    uint8_t  row_pins[16];  size_t n_rows;
    uint8_t  col_pins[16];  size_t n_cols;
    int32_t  matrix_to_code[16][16];   /* -1 where unmapped */
    int      i2c_bus;
    int      i2c_addr;
} nd_keymap;

nd_err   nd_keymap_load(const char *path, nd_keymap *out);   /* ND_ERR_NOTFOUND etc. */
int32_t  nd_keycode_for_name(const char *name);              /* -1 if unknown */
```

`matrix_to_code` as a fixed `16×16` array of `int32_t` is 1,024 bytes and removes every
hash lookup from the scan path.

### `nd_evdev.h` / `nd_input.h`

```c
nd_err  nd_evdev_discover(char *out_path, size_t out_sz);    /* the 6-step order */
int     nd_evdev_open(const char *path);                     /* O_RDONLY|O_NONBLOCK */
/* -1 when nothing was read; only EV_KEY value==1 produces a code */
int32_t nd_evdev_read_key(int fd, double timeout_s);
nd_err  nd_evdev_device_name(const char *path, char *out, size_t out_sz);

typedef struct nd_input nd_input;   /* matrix + evdev, in that order */
nd_err  nd_input_open(nd_input *in);
int32_t nd_input_read_key(nd_input *in, double timeout_s);   /* -1 = none */
int32_t nd_input_wait_key(nd_input *in);
bool    nd_input_has_matrix(const nd_input *in);             /* == _t9_active() */
void    nd_input_drain(nd_input *in);                        /* Browser's path */
```

### `nd_uinput.h` / `nd_t9_bridge.h`

```c
typedef struct { int fd; bool owns_device; } nd_uinput_kbd;

nd_err nd_uinput_open(nd_uinput_kbd *k, const char *path, const char *name);
nd_err nd_uinput_attach(nd_uinput_kbd *k, int fd);      /* tests: no ioctls */
nd_err nd_uinput_send_key(nd_uinput_kbd *k, uint16_t code, bool shift);
bool   nd_uinput_type_char(nd_uinput_kbd *k, char c);   /* false = untypeable */
nd_err nd_uinput_backspace(nd_uinput_kbd *k);
void   nd_uinput_close(nd_uinput_kbd *k);
bool   nd_uinput_char_to_keypress(char c, uint16_t *code, bool *shift);

typedef enum { ND_BRIDGE_SHELL, ND_BRIDGE_BROWSER } nd_bridge_kind;
typedef struct nd_t9_bridge nd_t9_bridge;

nd_t9_bridge *nd_t9_bridge_start(nd_bridge_kind kind, nd_input *in,
                                 nd_uinput_kbd *kbd);   /* NULL if no matrix */
void          nd_t9_bridge_handle_code(nd_t9_bridge *b, int32_t code);  /* testable */
void          nd_t9_bridge_stop(nd_t9_bridge *b);       /* joins, closes kbd */
```

### `nd_keypad_setup.h` / `nd_backlight.h`

```c
/* Called once at boot, before the UI is constructed. May execve() and never return. */
bool nd_keypad_setup_maybe_run(nd_fb *fb, bool restart);

typedef enum { ND_BL_PWM, ND_BL_GPIO, ND_BL_NONE } nd_bl_mode;
nd_bl_mode nd_backlight_mode(void);
bool       nd_backlight_available(void);
bool       nd_backlight_set_percent(int percent);
int        nd_backlight_get_percent(void);   /* -1 when unreadable */
bool       nd_backlight_off(void);
bool       nd_backlight_on(int percent);
```

---

## External dependencies and their C replacements

| Python dependency | Used for | C replacement |
| --- | --- | --- |
| `os.open` / `os.read` / `os.write` / `os.close` on `/dev/i2c-N` | PCF8575 transactions | `open`/`read`/`write`/`close` — identical |
| `fcntl.ioctl(fd, 0x0703, addr)` | I²C slave address | `ioctl(fd, I2C_SLAVE, addr)` from `<linux/i2c-dev.h>` |
| `os.open("/dev/uinput", O_WRONLY|O_NONBLOCK)` | virtual keyboard | same |
| `fcntl.ioctl` with `0x40045564`/`0x40045565`/`0x5501`/`0x5502` | uinput setup | `UI_SET_EVBIT`, `UI_SET_KEYBIT`, `UI_DEV_CREATE`, `UI_DEV_DESTROY` from `<linux/uinput.h>` |
| `struct.Struct("llHHi")` | `struct input_event` | the kernel struct directly; **16 bytes on arm32, 24 on x86-64** |
| `struct.Struct("80s4HI64i64i64i64i")` | `struct uinput_user_dev` | the kernel struct directly (1,116 bytes) |
| `struct.unpack('llHHI'/'IIHHI')` on evdev reads | key events | same struct; keep the 24/16 length branch for the host tests |
| `select.select` | evdev wait | `poll()` (better: one fd, no fd_set limit) |
| `os.fstat(...).st_size` | dictionary size | `fstat` |
| file seek/read/readline over `t9.dict` | binary search | `pread()` — no `FILE*`, no `stdio` buffer, no `getline` allocation |
| `time.monotonic()` | timeouts, multi-tap window | `clock_gettime(CLOCK_MONOTONIC)` |
| `time.time()` | uinput event stamps, `generated_at_unix` | `gettimeofday` / `time(NULL)` |
| `time.sleep` | settle and poll delays | `nanosleep` (`usleep` is fine for the daemon, which already uses it) |
| `threading.Thread` (daemon) + `threading.Event` | bridge polling thread | one `pthread` + an atomic `stop` flag; `pthread_join` with a 2 s deadline needs `pthread_timedjoin_np`, or use a self-pipe |
| `json.load` | keymap read | a ~200-line hand-rolled parser for this exact shape, **not** a JSON library. The document is flat, ≤ 2 KB, machine-generated, and the parser only needs objects, arrays, strings and ints. |
| `json.dump(indent=2, sort_keys=True)` | keymap write | hand-rolled writer emitting keys in sorted order with 2-space indent |
| `os.makedirs(exist_ok=True)` / `os.replace` / `os.fsync` | atomic keymap write | `mkdir` walk, `fsync(fd)`, `rename()` |
| `os.execv(sys.executable, [sys.executable] + sys.argv)` | wizard restart | `execv("/proc/self/exe", saved_argv)` |
| `glob.glob` | evdev discovery, backlight device search | `opendir`/`readdir` + `fnmatch`, results `qsort`ed with `strcmp` to match Python's `sorted()` |
| `os.path.realpath` | evdev de-duplication | `realpath()` |
| `os.environ.get` | `NEODCT_KEYPAD_DEVICE`, `NEODCT_KEYPAD_SETUP_BUS` | `getenv` |
| **PIL** `Image.new("RGB",(240,175),"black")` | wizard canvas | `nd_image_new(240, 175, ND_RGB888)` from the rasterizer |
| **PIL** `ImageDraw.Draw(...)`, `.rectangle`, `.text` | wizard drawing | `nd_draw_rect` (filled and outline forms), `nd_draw_text` |
| **PIL** `draw.textbbox((0,0), s, font)` | `_text_size` | `nd_text_bbox()` — must return the **same** `(x0,y0,x1,y1)` PIL does, because the wizard uses `bbox[3]-bbox[1]` as the height and `(240-w)//2` as the x |
| **PIL** `ImageFont.truetype(path, size)` | 26 / 18 / 14 px faces | FreeType, same `font.ttf`, same sizes |
| **PIL** `ImageFont.load_default()` | fallback when the font is missing | the rasterizer needs *some* built-in fallback face; on the real image `font.ttf` is always present, so this path is cosmetic. **Record in OPEN-QUESTIONS.** |
| `gpiozero` `Button` / `OutputDevice` | the non-i²c matrix backend | **recommend dropping** — see §5 |
| `argparse` | standalone `__main__` test entry points | plain `argv` walk, as `neodctDisplay.c` already does |

Nothing here needs a library that is not already in the C standard library or the Linux
UAPI headers. **No new Buildroot packages.**

### Memory notes for the C author

| Item | Python cost today | C cost |
| --- | --- | --- |
| `t9.dict` | 0 resident (already seeks) | 0 resident — keep it that way |
| `matrix_to_code` dict | ~1 KB of PyObjects | 1,024 B static array |
| `_held` / `_pending` | dict + list of tuples | 192 B + 64 B fixed arrays |
| T9 engine instance | ~500 B of PyObject | ~64 B struct, embeddable in the widget |
| Wizard canvas | 240×175×3 = 126,000 B PIL image + PIL itself | 126,000 B, freed when the wizard returns |
| uinput `_PLAIN`/`_SHIFTED` dicts | two Python dicts | one `int16_t[128]` lookup indexed by char, plus a parallel bitmap for "needs shift" — 256 B total, `O(1)` |
| Display daemon | already C, 283 KB | unchanged |

The single biggest memory item in this subsystem is the wizard's canvas, and it only
exists for the seconds the wizard runs on a phone that has never booted before. **Free
it before `execv`.**

---

## Proposed C modules

| File | Contents | Est. LOC |
| --- | --- | ---: |
| `nd_keycodes.h` | `MATRIX_NAME_TO_CODE`, `CODE_TO_DIGIT`, `PASSTHROUGH_CODES`, the Linux `KEY_*` values used, `nd_keycode_for_name()` | 80 |
| `nd_t9_engine.c/.h` | the multi-tap / predictive state machine, `char_allowed`, cycle tables | 230 |
| `nd_t9_dict.c/.h` | `digits_for`, `pread` binary search, `suggest`, the shared singleton | 200 |
| `nd_pcf8575.c/.h` | raw I²C open / `write16` / `read16` / close | 110 |
| `nd_matrix.c/.h` | `nd_matrix_scanner` (raw scan, edge detect, debounce, pending queue) + `nd_matrix_input` (`read_key`) | 230 |
| `nd_keymap.c/.h` | keymap JSON reader + writer, backend selection, all six refusal logs | 280 |
| `nd_evdev.c/.h` | device discovery, `_event_device_name`, event decode | 200 |
| `nd_input.c/.h` | matrix-then-evdev facade: `read_key`, `wait_key`, `drain`, `has_matrix` | 150 |
| `nd_uinput.c/.h` | virtual keyboard: device creation, event emission, char→keycode tables | 290 |
| `nd_t9_bridge.c/.h` | `T9ShellBridge` + `T9BrowserBridge`, the polling thread, start/stop | 280 |
| `nd_keypad_setup.c/.h` | `SetupScreen` drawing, `PairScanner`, `_bipartition`, `_build_payload`, `_save_keymap`, `run_wizard`, `maybe_run_first_time_setup` | 480 |
| `nd_backlight.c/.h` | three tiers, sysfs reads/writes, banker's rounding | 180 |
| `neodct_displayd` (`neodctDisplay.c`) | **existing C, kept**; warning cleanup only | 654 (+40) |
| **New C total (excluding the reused daemon)** | | **≈ 2,710** |
| **Total C in this subsystem** | | **≈ 3,400** |

Placement against the target architecture:

- **`libneodct.so`**: `nd_keycodes.h`, `nd_t9_engine`, `nd_t9_dict`, `nd_uinput`,
  `nd_t9_bridge`, `nd_backlight`. Apps type into text fields and the browser app starts
  a bridge, so all of these must be reachable from an app process.
- **core binary only**: `nd_pcf8575`, `nd_matrix`, `nd_keymap`, `nd_evdev`, `nd_input`,
  `nd_keypad_setup`. **Only the core may own the i²c bus and the evdev fd.** Two
  processes scanning the same PCF8575 would corrupt each other's row drive.
- **standalone binary**: `neodct_displayd`.

### The cross-process problem this creates

Today an app runs *inside* the UI process and reads keys through `ui.read_keypress()`.
With process-per-app the app cannot touch the i²c bus. Two options, and this needs a
decision before any app is ported:

1. **Core reads, pipes codes to the child.** The core keeps `nd_input`, and each app
   child gets a pipe fd on which key codes arrive as 4-byte integers. `nd_input` in
   `libneodct.so` grows a third backend (`ND_INPUT_PIPE`) so app code calls the same
   `nd_input_read_key()`. Costs one syscall per key. Koki needs held state, so the pipe
   must also carry release edges — which means the core must **synthesise** them from
   the scanner's `_held` transitions, something no code does today.
2. **Core hands the app the scanner.** Passes the i²c fd across the fork and lets the
   child scan while the core blocks in `waitpid()`. This is what the current design
   effectively does (the UI loop is blocked in `p.wait()` while LinuxShell runs, which
   is exactly why the T9 bridge is safe to start), but it means the core cannot see an
   incoming call's keypress while an app runs.

**Recommendation: option 1**, with the core synthesising press *and* release events onto
the pipe, since ARCHITECTURE.md already moves the modem onto its own thread and wants
the core awake while an app runs. Recorded in the risks below.

---

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| **Nothing in the test suite touches the PCF8575, the keymap loader, or the wizard.** The three most hardware-coupled files in the subsystem have zero coverage, so a port regression is invisible until a phone is in front of you. | **high** | Write host unit tests first, against a fake i²c fd (a `socketpair` or a temp file the test seeds with scan responses). Fake the chip, not the driver. |
| **The wizard's pixels are unverified.** `uistub.py` never runs it (it is called before `NeoDCT_UI` exists), so there is no golden frame and no test. A wrong font baseline or a one-pixel progress bar is not caught. | **high** | Capture golden frames from the Python wizard *before* porting: drive `SetupScreen` directly with a stub `fb` that saves the canvas, for each of the nine screen states listed in §10. This is ~40 lines of host code and it is the only way "same pixels" is checkable here. |
| **PIL `textbbox` semantics.** `_text_size` returns `bbox[3]-bbox[1]`, which is *not* the font's line height — it is the ink height of that specific string, and it changes with descenders. `_center` then draws at the raw `y` with PIL's default `"la"` anchor. Any deviation shifts every string in the wizard. | **high** | Make `nd_text_bbox()` byte-identical to PIL's for these three sizes before porting the wizard. This is a shared dependency with the UI framework spec — coordinate. |
| **The cross-process input question (above) has no answer yet.** Every app that reads keys depends on it, and Koki depends on held state that no current API exposes. | **high** | Decide before the first app port. Prototype the pipe with Koki, which is the hardest consumer. |
| **`t9.dict` is 2.88 MiB, not the 512 KiB the comments claim.** A C author who sizes a buffer from the docstring, or who decides "half a megabyte, might as well mmap it", loses 3 MB of a 53 MB phone. | medium | The spec states the real numbers. Use `pread`, never `mmap`, never a full read. Add a unit test that asserts `nd_t9_dict` allocates a bounded amount regardless of file size. |
| **`_line_at`'s 64-byte back-scan is only safe because words are ≤ 12 chars.** A future dictionary with longer entries silently returns a truncated word and the binary search misbehaves. | medium | Assert `MAX_LEN <= 12` in `mkt9dict.py` (already true), and in C treat "no newline found in the back-scan" as "start = back", exactly as the Python does. |
| **`round()` is banker's rounding** in `backlight.set_percent`. C's `round()` is round-half-away-from-zero. `round(127.5)` differs. | medium | Use `nearbyint()` with `FE_TONEAREST`, or write the two-line half-to-even helper. There is one call site. |
| **`struct fb_fix_screeninfo` offset bug.** `core/main.py:301` reads `line_length` at offset **48**, which is correct on x86-64 and reads `mmio_start` on arm32 (line_length is at 44 there). It survives only because vfb reports `mmio_start = 0` and the `if line_length == 0` fallback computes `xres * bpp/8` correctly. | medium | Use the real struct in C. **Note this as a fixed bug, not a silent change** — per CODING-STANDARDS §9.4 the quirk is invisible on screen, so fixing it is safe, but it must be written down. Belongs jointly to this spec and the core-loop spec. |
| **The bridge thread and `fork()`.** The T9 bridge is a running thread in the process that forks the shell / netsurf child. CODING-STANDARDS §1.1 bans a fork-without-immediate-exec from a threaded process; the current Python starts the bridge *after* `Popen`, which is the safe order. | medium | Preserve the ordering: fork+exec the child first, *then* start the bridge thread, and stop the thread before the next fork. Document it in the module header. |
| **`--speed 16` in `S90display` is 16 MHz, not 16 Hz** — the `< 1000 means MHz` rule. Innocuous today; a C rewrite that "cleaned up" the flag would break boot output. | low | Keep `parse_int` and the `< 1000` multiply verbatim. |
| **`_probe_chip` accepts any I²C device that ACKs at 0x20–0x27.** A future fuel gauge or RTC at 0x20 would be enrolled as a keypad. | low | Reproduce as-is; it is a real property of the PCF8575 (no ID register). Note in OPEN-QUESTIONS whether a "does the scan ever see a pair?" sanity check should be added. |
| **`gpiozero` matrix backend has no user on the target.** Porting it means reimplementing gpiozero's `Button`/`OutputDevice` over libgpiod for a path that cannot run on the phone. | low | Drop it, log `[INPUT] Keymap present, but driver '{driver}' is not supported.` when a keymap names a non-`pcf8575-i2c` driver, and raise the deviation in OPEN-QUESTIONS. |
| **Python `set` iteration order in `_bipartition`.** Affects only which conflict is reported first. | low | Use sorted neighbour lists; note the deviation. |
| **`_word_digits` is unbounded** in the Python. | low | Cap at 32 in C; nothing above 12 can match anyway. |
| **Duplicated evdev decoding** in `call_screen.py` and `Koki/engine.py`. Unifying them is tempting and would be a behaviour change (`call_screen` currently ignores the matrix backend). | low | Port the duplication; flag for a later cleanup. |

---

## Tests that cover this

Run from the repo root with `python3 -m pytest neodct/tests/ -q`.

| Test file | Tests | Covers | Usable as a port oracle? |
| --- | ---: | --- | --- |
| `test_t9_engine.py` | 27 | Every branch of `T9Engine.press` — default mode, cycling, digit-at-end-of-cycle, wrap, timeout expiry with an injected `FakeClock`, `0`/`1` cycles, star in each mode, nav-key commit, `reset()`, the full `#` cycle for all three filters, `char_allowed`. | **Yes — the best oracle in the subsystem.** Translate case-for-case into a C unit test. The `FakeClock` injection point already exists in the API (`clock=`), so the C `nd_clock_fn` mirrors it exactly. |
| `test_t9_dict.py` | 17 | `digits_for` (including the untypeable cases and the case-insensitivity that makes `NeoDCT` searchable), prefix search, ordering, the `MIN_PREFIX` refusal, non-digit refusal, `limit`, the missing-dictionary fallback, first/last-word boundaries, an exhaustive 240-word dictionary where every word must be findable, and two tests against **the real shipped `t9.dict`** (that `636328` → `NeoDCT` first, and that the whole 315,752-line file is sorted by key). | **Yes.** The exhaustive-findability test is the one that catches an off-by-one in the C binary search. Keep the shipped-file tests — they are cheap and they guard `mkt9dict --add`. |
| `test_t9_uinput.py` | 27 | `char_to_keypress` for lower/upper/digit/symbol/unknown; `UInputKeyboard` event emission read back through a **pipe** (`fd=` injection, no `/dev/uinput`, no root) including the "SYN after every key event" invariant and the shift-wrapping order; `T9ShellBridge` multi-tap, cycling backspace, passthrough commit, hash-silent mode change; thread start/stop lifecycle; `start_shell_bridge` returning `None` with no keypad and on keyboard-factory failure; the full browser bridge (arrows, 5=Enter, inert keys in cursor mode, the four-stop `#` cycle, pending-multitap reset on mode change). | **Yes.** The pipe-injection pattern (`nd_uinput_attach(fd)`) must survive into the C API or these tests cannot be ported. |
| `test_linuxshell_t9.py` | 3 | `LinuxShell._start_t9_bridge`: `None` without a keypad, `None` when uinput raises, and started/stoppable with a fake keyboard. | Partial — app-level wiring, belongs with the apps spec, but it pins the "uinput failure is not fatal" contract. |
| `test_mkt9dict.py` | 11 | House words winning their key, surviving a budget of 8 bytes, keeping their capitals; the build being sorted by key; common words outranking rare ones sharing a key; `--add` placement, idempotence, refusal of untypeable words, and that an added word is findable through the real `T9Dictionary` search. | Host tool only — **keep the Python tool and these tests unchanged.** |
| `test_framework_predictive.py` | 24 | The widget side of predictive: digits becoming a word, the provisional underline, `*` cycling candidates and wrapping, longer words replacing rather than extending, space/punctuation committing, mode change keeping the word, clear taking one digit off, clearing past the word deleting committed text, the no-dictionary fallback showing raw digits, the pencil indicator vs plain text, and no indicator at all on a dev keyboard. | Consumer — belongs to the UI framework spec, but it is the only test that exercises `pop_word_digit()` returning `None`. |
| `test_framework_text_input.py` | 17 | `TextInput` / `TextInputLong` T9 key handling, filters, the dev-keyboard path. | Consumer, same note. |
| `test_backlight.py` | ~20 | All three tiers against a sysfs tree built in `tmp_path`: PWM preferred, GPIO fallback, none; `MIN_ON_PERCENT` clamping; `ACTIVE_LOW`; export handling. | **Yes.** Trivially portable — point the C module's roots at a temp dir. |

### What has no coverage at all

- `pcf8575_keypad.py` — the scanner, the debounce, the rollover queue, `read_key`.
- `i2c_keypad_setup.py` — every line of it, including the graph 2-colouring and the
  JSON payload shape.
- `core/main.py` `_load_matrix_keymap`, `_discover_keypad_path`, `MatrixKeypadInput`,
  and the evdev decode in `read_keypress`.
- `neodctDisplay.c` — no test, only `--test` on real hardware.
- `neodct-sdcard` is covered by `test_sdcard_helper.py` (sourced with
  `NEODCT_SDCARD_SOURCE_ONLY=1`) but that is the storage subsystem's business.

**Every one of those should get a host test written before the C is written**, and each
is straightforwardly fakeable: the scanner needs a fake fd that returns scripted 16-bit
words, the keymap loader needs temp JSON files, and the wizard needs the same fake
scanner plus a canvas-capturing `fb`.

---

## How this could be split across agents

The subsystem decomposes cleanly because most of it has no shared state. Three waves.

### Wave 1 — four agents in parallel, no dependencies between them

| Agent | Modules | Why it is independent |
| --- | --- | --- |
| **A: T9 logic** | `nd_t9_engine`, `nd_t9_dict`, `nd_keycodes.h` | Pure logic and one read-only file. 27 + 17 existing tests to translate. Needs nothing but libc. **Highest-value first task** — it unblocks the UI framework agent. |
| **B: I²C and matrix** | `nd_pcf8575`, `nd_matrix`, `nd_keymap` | Talks only to `/dev/i2c-N` and one JSON file. Must write its own tests (none exist). Owns the fake-i²c harness the wizard agent will reuse. |
| **C: evdev and input facade** | `nd_evdev`, `nd_input` | Talks only to `/dev/input/*`. Depends on `nd_keycodes.h` (a header agent A produces early) and on `nd_matrix`'s *header* — coordinate the two signatures up front, then work in parallel. |
| **D: display daemon and backlight** | `neodctDisplay.c` warning cleanup, `nd_backlight` | Completely disjoint from everything else. Smallest task; can be the same agent as one of the above if headcount is tight. |

### Wave 2 — two agents, each needing one Wave-1 result

| Agent | Modules | Waits on |
| --- | --- | --- |
| **E: uinput and bridges** | `nd_uinput`, `nd_t9_bridge` | Agent A's `nd_t9_engine` API, and Agent C's `nd_input` for the `read_key` callback. The 27 existing tests make this well-specified. |
| **F: the wizard** | `nd_keypad_setup` | Agent B's `nd_pcf8575` and its fake-i²c harness, **and the rasterizer's `nd_draw_text` / `nd_text_bbox`** from the UI framework work package. This is the only module in the subsystem with a hard dependency outside it. |

### Wave 3 — one agent

| Agent | Work |
| --- | --- |
| **G: integration** | Wire `nd_input` into the core loop, resolve the cross-process input question (§ Proposed C modules), synthesise release edges for Koki, and capture the golden frames for the nine wizard screens. |

### Splitting rules that matter

- **Agent A must publish `nd_keycodes.h` on day one.** Five other modules include it and
  the numbers are non-negotiable, so it should be written, reviewed and frozen before
  anything else starts.
- **Agent B owns the fake-i²c test harness.** Agent F reuses it; do not let two agents
  write two fakes.
- **Agents E and F both draw on the T9 engine but never on each other.** No coordination
  needed beyond the engine header.
- **Nobody but Agent D touches `neodctDisplay.c`.** It is working code; the temptation
  to "tidy" it while passing through is the main risk to it.
- The wizard (F) is the single largest module (~480 LOC) and the only one with an
  unverifiable pixel contract. If one agent has to be given more time, give it to F, and
  give it the golden frames from Agent G's capture work *before* it starts.

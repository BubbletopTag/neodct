# Core process: framebuffer, keypad, main loop, NeoDCT_UI — C port specification

**Subsystem owner files**

| Python file | LOC | Role |
| --- | --- | --- |
| `neodct/overlay/NeoDCT/System/core/main.py` | 1317 | Framebuffer driver, keypad backends, `NeoDCT_UI`, the main loop |
| `neodct/overlay/NeoDCT/launcher.py` | 120 | Process entry point: serial redirect, clock, remote shell, boot splash |

Everything below was read from those two files at commit `4fbf85a8`. Every coordinate,
colour, key code and timeout quoted here was either read directly out of the source or
measured by driving the real UI through `neodct/tools/uistub.py`. Where a number was
measured rather than literally written in the Python, it says so.

---

## What this does (plain English)

This is the part of NeoDCT that *is* the phone. It is the program that starts when the
phone boots and keeps running until it is switched off. Everything else — the phone
book, messages, games — runs on top of it.

It does five jobs.

**1. It puts pixels on the screen.** The Linux kernel gives us a file, `/dev/fb0`, which
behaves like a chunk of memory: write bytes into it and they appear on the display. The
`Framebuffer` class asks the kernel two questions when it starts ("how wide and tall is
the screen?" and "how many bytes is one row?"), then maps that memory into the program so
writing to it is as cheap as writing to a normal array. The UI draws into an ordinary
240×175 image and then hands that image to the framebuffer, which converts it into the
exact byte layout the panel expects and copies it in.

The screen is 240×240 physically, but the UI only ever uses a 240×175 strip in the
middle — that is the "band". On the real phone a separate small C program
(`neodct_displayd`) already tells the kernel the framebuffer is 240×175 and puts the band
on the panel itself, so the Python usually just writes one contiguous block. The code can
also handle the case where the framebuffer really is 240×240, and then it centres the
band itself.

**2. It reads the keypad.** There are two ways keys can arrive. On a development board
there is a USB or emulated keyboard, and Linux gives us key presses through
`/dev/input/eventN` — a standard Linux mechanism. On the real phone the keypad is a grid
of wires (a "matrix") wired to a chip called a PCF8575 hanging off the I2C bus; there is
no kernel driver, so the code scans the grid itself, one row at a time, and works out
which button is down. Which of the two is used depends on whether a file called
`/NeoDCT/User/keymap.json` exists — that file is written once by a setup wizard the first
time the phone boots with a real keypad attached.

**3. It draws the home screen.** The home screen is not hard-coded. It is described by a
small JSON file, `/NeoDCT/System/ui/resources/ui_home.json`, which lists four things: a
battery gauge on the right, a signal-strength gauge on the left, a clock in the top-right
corner, and the carrier name in the middle. The code walks that list and draws each item.
Two items are special-cased by their placeholder text: an item whose text is `"12:00"`
gets replaced by the real clock, and an item whose text is `"No Service"` gets replaced by
the real carrier name if the modem knows one. At the bottom there is a 30-pixel strip
showing what the middle button does — "Menu" normally, "Call" when you have typed a
number, "Read" when a text message is waiting.

**4. It runs the state machine.** There are exactly three states. `HOME` is the idle
screen. `HOME_DIALING` is what you get the moment you press a digit — the digits appear
in big text in the middle and the middle button becomes "Call". `MENU` is the app picker.
Pressing the middle button on `HOME` goes to `MENU`; the menu blocks until you pick an app
or back out, then the app runs, and when the app finishes you are back on `HOME`.

**5. It notices things that must interrupt whatever is happening.** Every screen in the
whole system — home, menus, dialogs, apps — eventually calls `read_keypress()` to wait for
a button. That makes `read_keypress()` the one place every screen passes through, so three
extra jobs are bolted onto it: sample the battery (and shut the phone down at 3.20 V),
pump the modem (arriving texts become inbox rows and a notification), and check whether
the phone is ringing. If it is ringing, `read_keypress()` throws a special Python
exception called `IncomingCall`. Because it is an exception, it tears its way up out of
whatever app was running — and crucially, Python runs each app's cleanup code (`finally:`
blocks) on the way, which is how the music player and Koki hand the sound card back before
the ringtone starts. The 3310 did not multitask either.

In C there are no exceptions, so job 5 changes shape: apps will be separate processes and
the core will send them a signal instead. That is the one deliberate structural change in
this subsystem; everything else is a literal translation.

---

## Files and where they go in C

| Python | Lines | Purpose | C destination |
| --- | --- | --- | --- |
| `System/core/main.py` 1–77 | 77 | Constants, `MATRIX_NAME_TO_CODE`, gpiozero probe | `nd_core.h`, `nd_input.c` |
| `System/core/main.py` 80–89 | 10 | `_setting_is_enabled` | `nd_settings.c` (shared with settings agent) |
| `System/core/main.py` 91–144 | 54 | `_event_device_name`, `_discover_keypad_path`, `_gpio_available` | `nd_input.c` |
| `System/core/main.py` 150–214 | 65 | `_load_matrix_keymap` | `nd_keymap.c` |
| `System/core/main.py` 217–290 | 74 | `MatrixKeypadInput` (gpiozero backend) | `nd_input_gpio.c` |
| `System/core/main.py` 292–439 | 148 | `Framebuffer` | `nd_fb.c` / `nd_fb.h` |
| `System/core/main.py` 441–498 | 58 | `init_databases` | `nd_dbinit.c` |
| `System/core/main.py` 500–513 | 14 | `IncomingCall` | `nd_core_loop.c` (becomes a flag + signal) |
| `System/core/main.py` 516–651 | 136 | `NeoDCT_UI.__init__`, `_scan_apps_from_dir` | `nd_ui.c`, `nd_applist.c` |
| `System/core/main.py` 677–758 | 82 | `load_layout`, `load_wallpaper`, `get_image`, `_cache_put` | `nd_layout.c`, `nd_imgcache.c` |
| `System/core/main.py` 760–843 | 84 | `get_text_size`, `render_element`, `_get_status_icon`, `_draw_status_label` | `nd_ui_home.c` |
| `System/core/main.py` 845–906 | 62 | `render_home`, `render_home_dialing` | `nd_ui_home.c` |
| `System/core/main.py` 907–966 | 60 | `launch_app`, `render_menu`, `update` | `nd_applaunch.c`, `nd_ui.c` |
| `System/core/main.py` 968–1054 | 87 | `_battery_tick`, `_modem_tick`, `_handle_modem_event`, `_store_incoming_sms` | `nd_ui_ticks.c` |
| `System/core/main.py` 1055–1113 | 59 | `_ring_tick`, `handle_incoming_call` | `nd_ui_ticks.c`, `nd_core_loop.c` |
| `System/core/main.py` 1114–1190 | 77 | `_play_dtmf`, `_count_unread_sms`, `_open_notification`, `_shutdown_low_battery`, `show_pending_battery_warning` | `nd_ui.c` |
| `System/core/main.py` 1191–1282 | 92 | `read_keypress`, `wait_for_key`, `handle_input` | `nd_ui_input.c` |
| `System/core/main.py` 1284–1317 | 34 | `run`, `__main__` | `nd_core_loop.c` |
| `launcher.py` 1–47 | 47 | `_redirect_stdio_to_serial`, `splash_version` | `nd_main.c`, `nd_log.c` |
| `launcher.py` 49–83 | 35 | `show_boot_logo` | `nd_splash.c` |
| `launcher.py` 85–120 | 36 | `main()` boot order | `nd_main.c` |

---

## Behaviour that must be reproduced exactly

### 0. Constants (verbatim)

```c
#define FB_PATH              "/dev/fb0"
#define KEYPAD_PATH          "/dev/input/event0"
#define KEYPAD_DEVICE_ENV    "NEODCT_KEYPAD_DEVICE"
#define KEYMAP_PATH          "/NeoDCT/User/keymap.json"
#define UI_WIDTH             240
#define UI_HEIGHT            175
#define SOFTKEY_HEIGHT       30
#define WIDTH                UI_WIDTH        /* alias, same value */
#define HEIGHT               UI_HEIGHT       /* alias, same value */
#define WALLPAPER_PATH       "/NeoDCT/User/wallpaper.jpg"
/* SERIAL_CONSOLE_DEVICE = getenv("NEODCT_SERIAL_DEVICE") or "/dev/ttyAMA0" */
#define FONT_PATH            "/NeoDCT/System/ui/resources/fonts/font.ttf"
#define HOME_LAYOUT_PATH     "/NeoDCT/System/ui/resources/ui_home.json"
#define ENVELOPE_PATH        "/NeoDCT/System/ui/resources/img/envelope.png"
#define APPS_DIR             "/NeoDCT/System/apps"
#define ENG_APPS_DIR         "/NeoDCT/System/engineering/apps"
#define DTMF_DIR             "/NeoDCT/System/tones/dtmf"
#define IMAGE_CACHE_MAX      32
```

Derived at construction: `content_bottom = HEIGHT - SOFTKEY_HEIGHT = 145`.

`MATRIX_NAME_TO_CODE` — the name→Linux-keycode table used when parsing `keymap.json`.
Note two names alias two codes (`navikey`/`enter` = 28, `clear`/`back` = 14):

```
navikey 28   clear 14   up 103   down 108   left 105   right 106
menu 50      enter 28   back 14
num_1 2  num_2 3  num_3 4  num_4 5  num_5 6
num_6 7  num_7 8  num_8 9  num_9 10 num_0 11
star 42  hash 43
```

`NeoDCT_UI.DEV_KEYMAP` — evdev keycode → dialled character:

```
2:'1' 3:'2' 4:'3' 5:'4' 6:'5' 7:'6' 8:'7' 9:'8' 10:'9' 11:'0'
12:'-' 52:'.' 51:',' 42:'*' 43:'#' 28:'#'
```

The `28:'#'` entry is **dead code**: code 28 is consumed by an earlier branch in
`handle_input`. Port the table as written; do not "clean it up".

`_setting_is_enabled(value, default=True)`: `None` → default; lowercase-strip the string;
`"1" "true" "on" "yes" "enabled"` → true; `"0" "false" "off" "no" "disabled"` → false;
anything else → default.

---

### 1. Boot sequence (`launcher.py:main`)

Order is load-bearing; every step's failure is caught and boot continues.

1. **`_redirect_stdio_to_serial()`** — pick the console: `/dev/ttyFIQ0` if it exists
   (real Rockchip/Luckfox), else `/dev/ttyAMA0` (QEMU PL011), else
   `SERIAL_CONSOLE_DEVICE`. Open it for writing and point stdout and stderr at it.
   **Only after the redirect** install `System.core.logstyle` colouring — it must wrap
   the new stream, not the one just replaced. Logs `[Launcher] Serial console active:
   <dev>` on success, `[Launcher] Serial redirect failed for <dev>: <exc>` on failure.
2. **`ClockService.start()`** — applies a synchronous clock floor before anything can
   reach the network (a 1970 clock fails every TLS "not valid before" check). On failure:
   `[CLOCK] clock service unavailable: <exc>`.
3. **`RemoteShell.start_if_enabled()`** — on failure `[RSHELL] remote shell unavailable:
   <exc>`.
4. `print("[Launcher] Initializing Hardware...")`, then **construct the `Framebuffer`**.
5. **`show_boot_logo(fb)`**, then **sleep exactly 1.0 s**.
6. `print("[Launcher] Starting UI...")`, then `ui_engine.run(fb)`.

`run(fb)` (`main.py:1284`) itself does, in order:

1. `System.hw.i2c_keypad_setup.maybe_run_first_time_setup(fb)` inside try/except; on any
   exception print `[SETUP] First-time keypad setup failed; continuing boot.` and the
   traceback, then carry on. **This call may never return** — if the wizard writes a
   keymap it calls `os.execv(sys.executable, [sys.executable] + sys.argv)` and restarts
   the whole UI process. In C the equivalent is `execv(/proc/self/exe, argv)`.
   The wizard runs only when *all* of: `/NeoDCT/User/keymap.json` is absent; and
   (`/dev/i2c-<bus>` exists, or `/dev/ttyFIQ0` exists) where bus is
   `getenv("NEODCT_KEYPAD_SETUP_BUS")` or 3; and a PCF8575 answers on that bus.
2. `ui = NeoDCT_UI(fb)`
3. `print("[CORE] Entering Main Loop...")`
4. The loop (see §12).

**Boot splash (`launcher.py:show_boot_logo`), exact geometry.** Canvas is a fresh
`RGB (UI_WIDTH, UI_HEIGHT)` = 240×175 filled black. Fonts are **DejaVu, not the NeoDCT
font**: `/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf` at 20 and
`/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf` at 14; on failure both fall back to
Pillow's built-in default font.

```
title_y = max(20, int(screen_h * 0.35))            = max(20, 61) = 61
title   = "Starting NeoDCT..."      bold 20, white, x = (240 - w) // 2, y = 61
ver     = "System v" + get_setting("system.os.versionnumber", "") or "?"
                                    regular 14, gray,  x = (240 - w) // 2, y = title_y + 30 = 91
```

`w` is `textbbox((0,0), text)[2] - [0]`. Measured against the DejaVu shipped on this
host: title w = 208 → x = 16; `"System v0.3.13a"` w = 118 → x = 61; `"System v?"` w = 72
→ x = 84. **These widths depend on the exact DejaVu build in the image** — recompute
them against the shipped font, do not hard-code the host's numbers.

The splash is pushed with a single `fb.update(canvas)`. `"gray"` is Pillow's named colour
`#808080` = (128,128,128).

---

### 2. `Framebuffer` (`main.py:292`)

#### Construction

```
fd = open("/dev/fb0", O_RDWR)

vinfo = ioctl(fd, 0x4600, 160-byte buffer)     /* FBIOGET_VSCREENINFO */
xres = u32 @ 0
yres = u32 @ 4
bpp  = u32 @ 24

finfo = ioctl(fd, 0x4602, 64-byte buffer)      /* FBIOGET_FSCREENINFO */
line_length = u32 @ 48
if line_length == 0: line_length = xres * (bpp // 8)

size            = line_length * yres
mm              = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)
bytes_per_pixel = max(1, bpp // 8)
stride_pixels   = line_length // bytes_per_pixel
```

Then **the entire mapping is zeroed once** (`mm.write(b"\x00" * size)`), which is what
makes later partial-band writes safe — the letterbox rows above and below the band are
black from boot and never written again.

> **Correctness note the C port must handle.** `u32 @ 48` is the offset of
> `line_length` in `struct fb_fix_screeninfo` **on 64-bit** only. On the 32-bit ARM
> target `line_length` is at offset **44** and offset 48 is `mmio_start`. The Python is
> saved by the `== 0` fallback, because the Rockchip fbdev reports `mmio_start == 0` and
> the fallback then computes the correct `xres * bpp/8`. In C, include
> `<linux/fb.h>` and read `finfo.line_length` from the real struct — **and keep the
> `if (line_length == 0) line_length = xres * bpp/8;` fallback**, because that is the
> value the Python actually ends up using on hardware and any driver with row padding
> would change the rendering. Flagged in Open Questions.

**Pixel-path detection, done once at construction.** With `bpp == 16`, try
`Image.new("RGB",(1,1)).tobytes("raw","BGR;16")`; success sets `_has_bgr16`. Pillow ≥ 11
removed that packer, so on the current image `_has_bgr16` is **False** (verified on this
host: `No packer found from RGB to BGR;16`).

The log line is exactly:

```
[FB] {xres}x{yres} @ {bpp}bpp, pixel path: {path}
```

where `path` is one of:

* `"BGRA 32bpp (C, fast)"` when `bpp == 32`
* `"BGR;16 16bpp (C, fast)"` when 16bpp and `_has_bgr16`
* `"PYTHON RGB565 PACK 16bpp (SLOW -- ~350ms/frame on RV1103!)"` otherwise

On the slow path it additionally prints
`[FB] WARNING: on hardware, ensure neodct_displayd v2.1+ runs BEFORE the UI so the
framebuffer is switched to 32bpp.` and allocates, **only on that path**:

* `_r565[i] = (i & 0xF8) << 8` for i in 0..255
* `_g565[i] = (i & 0xFC) << 3`
* `_b565[i] = i >> 3`
* `_rgb565_out` = `bytearray(size)`
* `_rgb565_band_out` = `bytearray(xres * yres * 2)`

On the 32bpp path none of those exist (the comment explicitly notes this saves ~350 KB
on a 64 MB device).

#### Pixel formats (verified byte-for-byte)

* **32bpp**: `src.convert("RGBA").tobytes("raw","BGRA")` → memory order **B, G, R, A**,
  alpha always 255. Red (255,0,0) → `00 00 ff ff`. That is `XRGB8888` read as a
  little-endian `uint32`.
* **16bpp**: `rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)`, written
  **low byte first** (little-endian). Red → `00 f8`, green → `e0 07`, blue → `1f 00`,
  white → `ff ff`, (18,52,86) → `aa 11`. The `BGR;16` fast path produces the identical
  bytes; a single C packer covers both.
* Any other bpp: raw RGB888 bytes, straight from the composed image.

#### `update(pil_image)`

```
src    = image (converted to RGB only if not already RGB)
copy_w = min(src.width,  xres)
copy_h = min(src.height, yres)
src_x  = max(0, (src.width  - copy_w) // 2)
src_y  = max(0, (src.height - copy_h) // 2)
cropped = src, unless a crop is actually needed
dst_x  = max(0, (xres - copy_w) // 2)
dst_y  = max(0, (yres - copy_h) // 2)
```

**Fast path** — taken when `src_x == 0 && src_y == 0 && copy_w == src.width &&
copy_h == src.height`, i.e. the source fits entirely inside the framebuffer:

* 16bpp: pack the whole source into RGB565 and call `_write_center_band`.
* 32bpp: `src.convert("RGBA").tobytes("raw","BGRA")` and call `_write_center_band`.
* Any other bpp falls through to the slow path.

**Slow path** — a persistent `native_img` of `RGB (stride_pixels, yres)` is created on
first use, cleared to black by pasting `(0,0,0)` over `(0, 0, stride_pixels, yres)`, the
cropped source is pasted at `(dst_x, dst_y)`, the whole thing is packed, and written from
byte 0 with `mm.write(data if len(data) == size else data[:size])`.

`_write_center_band(band, copy_w, copy_h, dst_x, dst_y)`:

```
row_bytes = copy_w * bytes_per_pixel
if dst_x == 0 and row_bytes == line_length:
    single write of copy_h*row_bytes at offset dst_y*line_length
else:
    for each of copy_h rows:
        write row_bytes at (dst_y*line_length) + (dst_x*bytes_per_pixel) + row*line_length
```

**What actually happens on the shipped devices.** Both QEMU (`video=Virtual-1:240x175M`,
`neodct/tools/run_qemu.sh:129,140`) and hardware (`neodct_displayd` forces
`FBIOPUT_VSCREENINFO` to 240×175 @ 32bpp, `System/hw/neodctDisplay.c:328-338`) give a
**240×175 32bpp framebuffer**. So the live case is: `copy_w=240, copy_h=175, dst_x=0,
dst_y=0, row_bytes = 960 = line_length` → one contiguous 168,000-byte write per frame.
The centring arithmetic exists for a genuine 240×240 framebuffer and must still be
ported: `dst_y` would then be `(240-175)//2 = 32`, which is the value
`tests/test_uistub.py::test_device_frame_band_starts_at_row_32` asserts.

> **Discrepancy to know about.** `uistub.CapturingFramebuffer.device_frame()` letterboxes
> the band **centred** (band top at panel row 32). The real panel is fed by
> `neodct_displayd` with `--yoff` defaulting to `PANEL_H - FB_H = 65`, i.e.
> **bottom-aligned**, top 65 rows black. Golden-frame comparisons happen at the band
> level (240×175), so this does not affect them, but do not "fix" either side.

---

### 3. Keypad discovery (`_discover_keypad_path`, `main.py:102`)

Priority order, with the exact log lines:

1. `getenv("NEODCT_KEYPAD_DEVICE")`, stripped. If non-empty **and the path exists**, use
   `realpath()` of it and log
   `[INPUT] Using NEODCT_KEYPAD_DEVICE: <resolved> (<name>)`.
   If non-empty and missing, log `[INPUT] NEODCT_KEYPAD_DEVICE not found: <override>` and
   continue.
2. `sorted(glob("/dev/input/by-path/*-kbd"))`, then `sorted(glob("/dev/input/by-id/*-kbd"))`,
   then `/dev/input/event0` if it exists. Walk that list in order, `realpath()` each,
   skip duplicates (a `seen` set of resolved paths), and take the first whose resolved
   path exists. Log `[INPUT] Selected keyboard device: <resolved> (<name>)`.
3. `sorted(glob("/dev/input/event*"))` — take `realpath(list[0])`, log
   `[INPUT] Fallback input device: <fallback> (<name>)`.
4. Nothing at all: log `[INPUT] No input event device found; defaulting to
   /dev/input/event0` and return `KEYPAD_PATH`.

`_event_device_name(path)` reads `/sys/class/input/<basename(realpath(path))>/device/name`
and strips it; any failure yields the literal string `"unknown"`.

`_gpio_available()` is `len(glob("/dev/gpiochip*")) > 0`.

---

### 4. Keymap loading (`_load_matrix_keymap`, `main.py:150`)

Returns `NULL` (Python `None`) — meaning "no matrix keypad" — in every failure case.

1. If the file does not exist, return None silently.
2. Parse JSON. On failure log `[INPUT] Keymap read failed (<path>): <exc>`, return None.
3. Require `row_pins` (list), `col_pins` (list), `keys` (object). Otherwise log
   `[INPUT] Keymap ignored (missing matrix fields): <path>`, return None.
4. Coerce every pin to int; on failure `[INPUT] Keymap ignored (invalid pin list): <exc>`.
5. Build `matrix_to_code`: for each `name -> entry` in `keys`, skip non-object entries,
   skip names not in `MATRIX_NAME_TO_CODE`, read `entry["row"]` and `entry["col"]` as
   ints (skip the entry on failure), and set `matrix_to_code[(row, col)] = code`.
6. If `matrix_to_code` is empty: `[INPUT] Keymap ignored (no recognized keys): <path>`.
7. `i2c_addr` from `payload["i2c_addr"]`, default `0x20`. If it is a string, parse as hex
   when it starts with `0x`/`0X`, else decimal. `i2c_bus` from `payload["i2c_bus"]`,
   default `3`. On failure `[INPUT] Keymap ignored (invalid i2c fields): <exc>`.
8. Return `{path, format (default "unknown"), driver (default "gpiozero-matrix"),
   row_pins, col_pins, matrix_to_code, i2c_bus, i2c_addr}`.

The file the wizard writes (`System/hw/i2c_keypad_setup.py:216`) has
`"format": "neodct.keymap.v3.matrix.i2c"`, `"driver": "pcf8575-i2c"`, plus
`generated_at_unix`, `output`, `by_matrix`, `by_code` which this loader ignores. Each
`keys[name]` object carries `label`, `row`, `col`, `row_pin`, `col_pin`; only `row` and
`col` are read here.

---

### 5. `MatrixKeypadInput` — gpiozero backend (`main.py:217`)

Used only when the keymap's `driver` is **not** `"pcf8575-i2c"`, gpiozero imported
successfully, and at least one `/dev/gpiochip*` exists.

Construction: `rows = [OutputDevice(pin, initial_value=True) for pin in row_pins]`,
`cols = [Button(pin, pull_up=True) for pin in col_pins]`. State: `_held` (set of
`(row,col)`), `_pending` (FIFO list), `_last_unmapped`.

`_scan_once()`:

```
current = {}
for row_idx, row in enumerate(rows):
    row.off()                 /* drive low */
    sleep(0.001)              /* 1 ms settle, per row */
    for col_idx, col in enumerate(cols):
        if col.is_pressed: current.add((row_idx, col_idx))
    row.on()                  /* release */

new = sorted(current - _held)
_held = current
if new:
    _pending.extend(new[1:]);  pressed = new[0]
elif _pending:
    pressed = _pending.pop(0)
else:
    if not current: _last_unmapped = None
    return None

code = matrix_to_code.get(pressed)
if code is None:
    if pressed != _last_unmapped:
        _last_unmapped = pressed
        print("[INPUT] Matrix key {pressed} has no mapping in {path}")
    return None
_last_unmapped = None
return code
```

Note the full-matrix scan (key rollover) and that extra simultaneous presses are queued,
one delivered per subsequent call. There is **no release debounce** in this backend — that
exists only in the PCF8575 one.

`read_key(timeout)`: `timeout = max(0.0, timeout)`, `deadline = monotonic() + timeout`,
then loop `_scan_once()` → return non-None immediately; if `monotonic() >= deadline`
return None; else `sleep(0.005)`.

`close()`: for each row `on()` then `close()` (each in its own try/except), then close
each column.

**The I2C backend** (`System/hw/pcf8575_keypad.I2CMatrixKeypadInput`) is the one used on
real hardware and belongs to the hw agent's spec; the core only needs its contract:
constructed from the same cfg dict, exposes `read_key(timeout) -> keycode | None` and
`close()`. Its scan drives one row pin low over raw 2-byte I2C writes (`I2C_SLAVE`
ioctl `0x0703`), sleeps 0.0005 s, reads 16 bits, and applies `RELEASE_SCANS = 3`
consecutive-miss release debounce.

---

### 6. `NeoDCT_UI.__init__` — construction order (`main.py:517`)

The order matters because later steps depend on earlier ones and because the first-run
security notice is drawn from inside the constructor.

1. `init_databases()` (see §11).
2. `self.modem = ModemService()`, `self.battery = BatteryService()`,
   `self.notify = NotifyService()`.
3. `self._unread_sms = self._count_unread_sms()`.
4. `self._handling_call = False`, `self._ring_seen_at = None`,
   `self._shutting_down = False`, `self.dial_buffer = ""`.
5. `DEV_KEYMAP`, then `W = 240`, `H = 175`, `SOFTKEY_H = 30`,
   `content_bottom = 145`.
6. `keypad_fd = None`, `keypad_path = None`, `matrix_input = None`.
7. **Matrix keypad selection**: `_load_matrix_keymap(KEYMAP_PATH)`. If it returned a cfg:
   * `driver == "pcf8575-i2c"`: if `/dev/i2c-<bus>` is missing, log
     `[INPUT] Keymap wants pcf8575-i2c, but /dev/i2c-<bus> does not exist.`; else import
     `System.hw.pcf8575_keypad.I2CMatrixKeypadInput`, construct it, and log
     `[INPUT] I2C matrix input active from <path> (bus=<b> addr=0x<AA> rows=<list>
     cols=<list>).` (address `%02X`). On exception: `matrix_input = None` and
     `[INPUT] I2C matrix init failed; falling back to evdev: <exc>`.
   * else if gpiozero failed to import: `[INPUT] Keymap present, but gpiozero is
     unavailable: <import error>`.
   * else if no `/dev/gpiochip*`: `[INPUT] Keymap present, but no /dev/gpiochip* devices
     were found.`
   * else construct `MatrixKeypadInput` and log `[INPUT] Matrix input active from <path>
     (rows=<list> cols=<list>).`; on exception `[INPUT] Matrix init failed; falling back
     to evdev: <exc>`.
8. **Evdev**: `keypad_path = _discover_keypad_path()`; `open(path, O_RDONLY|O_NONBLOCK)`.
   On failure log `[INPUT] Failed opening <path>: <e>`; if the path was not already
   `KEYPAD_PATH`, log `[INPUT] Falling back to /dev/input/event0` and retry that; if that
   also fails log `[INPUT] Evdev fallback failed: <e2>` and leave `keypad_fd = None`.
   If open succeeded: `[INPUT] Listening on <path>`. If both backends are absent:
   `[INPUT] WARNING: no active input backend.`
9. `self.softkey = SoftKeyBar(self)` — **before** `self.softkey` exists as an attribute,
   which is exactly how `SoftKeyBar` decides it is the *system* bar and therefore
   transparent (`framework.py:465`, `is_transparent = not hasattr(ui, 'softkey')`).
   Every `SoftKeyBar` an app builds later sees the attribute and is opaque. Reproduce
   this as an explicit boolean, not by re-deriving the trick.
10. `self.fb = fb_driver`; `self.canvas = Image.new("RGB", (240,175), "black")`;
    `self.draw = ImageDraw.Draw(self.canvas)`.
11. `self.state = "HOME"`.
12. **Fonts** from `FONT_PATH`: `font_s = 14`, `font_md = 18`, `font_n = 20`,
    `font_xl = 24`. Success → `[UI] Custom font loaded.`; any exception → all four become
    Pillow's `load_default()` and `[UI] Font load failed, using default.`
13. `show_alpha_security_notice_once(self)` — a blocking modal on first boot. It checks
    `/NeoDCT/User/.ack_security_warning`; if absent it shows a `MessageDialog` with
    title `"Notice"`, icon `/NeoDCT/System/ui/resources/img/errorscreen/warning.png`,
    button `"OK"`, and the message *"This is alpha software. Consider it extremely
    insecure and unstable. Don't store important data on this device."* — then writes
    `str(int(time.time()))` into the ack file. Note this runs **before**
    `self.engineering_mode`, `self.home_layout`, `self.wallpaper` and `self.apps` exist.
14. `self.home_layout = self.load_layout(HOME_LAYOUT_PATH)` — JSON or `None` on any error.
15. `self.image_cache = {}`.
16. **Wallpaper**: `wallpaper_setting = get_setting("system.ui.wallpaper", "NONE")`.
    If it is truthy and `upper() != "NONE"`:
    * if it lowercase-ends with `.jpg` or `.jpeg` **and** the file exists → that is the
      path;
    * otherwise log `[UI] Invalid wallpaper setting: <setting>` and, if
      `/NeoDCT/User/wallpaper.jpg` exists, use that instead.

    Then `self.wallpaper = self.load_wallpaper(path)` if a path was chosen, else `None`.
17. `self.engineering_mode = _setting_is_enabled(get_setting("system.ui.engineering_mode",
    "ON"), default=True)`.
18. **App scan**: `app_dirs = ["/NeoDCT/System/apps"]`, plus
    `"/NeoDCT/System/engineering/apps"` when engineering mode is on. Scan each, then
    `self.apps.sort(key=lambda x: x["id"])`. Any exception anywhere in the scan block:
    `[OS] App scan error: <e>` and the partially-built list is kept.

`_scan_apps_from_dir(dir)`: if the directory does not exist, `os.makedirs` it (and return
silently on failure). For each entry in `os.listdir(dir)` (note: **unsorted**, filesystem
order — the later sort by `id` is what makes the menu deterministic), read
`<dir>/<folder>/manifest.json` if present and append
`{"name": data.get("name", folder), "icon": "<dir>/<folder>/" + data.get("icon","icon.png"),
"path": "<dir>/<folder>", "exec": data.get("exec","main.py"), "id": int(data.get("id",999))}`.
Any per-app exception is swallowed.

Shipped manifests (id → name): 1 Phone book, 2 Messages, 3 Call Log, 4 Settings,
6 Games, 7 Calculator, 8 Clock, 9 Tones, 10 Koki Mobile, 11 Browser, 12 Update,
970 Music, 971 Power; engineering: 999 Linux Shell, 9001 LCD Test, 9002 KeyMap,
9003 KeyMapI2C, 9004 FuelGauge, 9005 ModemInfo, 9006 Downgrade, 9990 Remote Shell,
9997 Crash, 9998 Cube Bench, 9999 Tests. 24 entries with engineering on.

---

### 7. Fonts and text measurement

The font is **`Nokia Cellphone FC Small`** (16,272-byte TTF at
`/NeoDCT/System/ui/resources/fonts/font.ttf`). Four sizes are loaded: 14, 18, 20, 24.
`font_md` (18) is loaded but **never used by anything in `main.py`** — apps use it.

`get_text_size(text, font)` returns `(bbox[2]-bbox[0], bbox[3]-bbox[1])` where `bbox` is
`draw.textbbox((0,0), text, font=font)`. This is the single measurement primitive the
entire UI centres text with, so it must match Pillow exactly.

Measured properties of this font (all four sizes behave identically after scaling):

* `getmetrics()` = `(ascent, descent)` = `(size, size/4)` — 14→(14,4), 18→(18,5),
  20→(20,5), 24→(24,6).
* **Advance widths are exactly `n * size / 8`** with n from this table:

  | n | characters |
  | --- | --- |
  | 2 | `'` |
  | 3 | space `!` `,` `.` `:` `;` `I` `` ` `` `i` `l` `\|` |
  | 4 | `"` `(` `)` `/` `1` `[` `\` `]` `^` `f` `j` `t` `{` `}` |
  | 5 | `-` `<` `=` `>` `J` `L` `S` `c` `r` `s` |
  | 6 | `#` `$` `*` `+` `0` `2` `3` `4` `5` `6` `7` `8` `9` `?` `A`–`H` `P` `R` `U` `Z` `_` `a` `b` `d` `e` `g` `h` `k` `n` `o` `p` `q` `u` `v` `x` `y` `z` `~` |
  | 7 | `%` `&` `@` `K` `N` `O` `Q` `T` `V` `X` `Y` |
  | 8 | `M` `W` `w` |
  | 9 | `m` |

  So at size 20 a digit advances 15.0 px and a space 7.5 px. **Advances are fractional
  and must be accumulated in fixed point, then the string width rounded up once at the
  end.** `"Tello"` at 14 sums to 43.75 → reported width 44. `"No Service"` sums to
  91.0 → 91. Rounding each glyph individually gives the wrong answer.
* Reported **width** = `ceil(sum of advances)`; a string of only spaces still reports a
  positive width (`" "` at 20 → 8) but zero height.
* Reported **height** = ink bottom − ink top, with the baseline at `y = ascent`:
  * caps, digits and tall lowercase: top = `size * 1/8` (rounded), bottom = `size`
    (e.g. size 20 → top 2, bottom 20, height 18);
  * x-height lowercase and `:` `;`: top = `size * 7/20`-ish (size 20 → 7);
  * `-` and `=`: size 20 → top 9;
  * `,` `.`: size 20 → top 15; `_`: top 17;
  * descenders `$ ( ) , ; @ Q [ ] g j p q y { |  }`: bottom = `size + size/8`
    (size 20 → 23).

  Concrete values used by the home screen: `"Tello"` @14 → (44, 13); `"17:08"` @14 →
  (44, 13); `"No Service"` @14 → (91, 13); `"Menu"` @20 → (65, 18); `"Call"` @20 →
  (45, 18); `"Read"` @20 → (60, 18); `"?"` @14 → (11, 13); `"3 messages"` @20 → (143, 21);
  `"received"` @20 → (108, 18); `"0821234567"` @24 → (174, 21).

`draw.text((x,y), ...)` uses Pillow's default anchor `"la"`: **`y` is the ascender top**,
so the baseline lands at `y + ascent`. Reproduce that, not a baseline origin.

The pragmatic C approach is to keep FreeType with the same TTF at the same four sizes (the
architecture doc already commits to this), and to implement `nd_text_size()` as
`ceil(Σ advance)` × ink-extent, matching the table above. Baking per-size bitmap glyph
tables offline from the same TTF is an acceptable alternative and would remove FreeType
from the runtime entirely — see Risks.

---

### 8. The home screen, pixel by pixel

`self.canvas` is a persistent 240×175 RGB image. It is **never cleared wholesale by the
core loop** — each render path paints its own background first.

`ui_home.json` as shipped (background `null`, four elements, in this order):

| # | type | fields |
| --- | --- | --- |
| 1 | `icon_set` | `count 5, prefix "bat", x 210, y 24, sim_val 4, custom_images {"0".."4" → /NeoDCT/System/ui/resources/img/battery/bat-N.png}` |
| 2 | `text` | `text "No Service", font_size 12, anchor "center_h", x 120, y 71, color "white"` |
| 3 | `text` | `text "12:00", font_size 8, anchor "right", x 213, y 12, color "white"` |
| 4 | `icon_set` | `count 5, prefix "sig", x 7, y 24, sim_val 4, custom_images {"0".."4" → .../cellsignal/sig-N.png}` |

#### Coordinate mapping (`render_element`)

```
x = int((el.x / 240.0) * W)      /* W = 240, so x is unchanged */
y = int((el.y / 240.0) * H)      /* H = 175, so y = floor(el.y * 175 / 240) */
```

Resolved: y 24 → **17**, y 71 → **51**, y 12 → **8**.

#### Text elements

* Substitutions, matched on the literal placeholder string:
  * `text == "12:00"` → `strftime("%H:%M")` (24-hour, local time).
  * `text == "No Service"` → `modem.operator_display()` **if it is truthy**; a `None`
    or empty result leaves the placeholder.
* Font choice: `font_size >= 20 → font_xl(24)`, `>= 16 → font_n(20)`, else `font_s(14)`.
  The shipped layout's 12 and 8 both mean **font_s, 14 px**.
* `w, h = get_text_size(text, font)`.
* Anchor: `"center_h" in anchor` → `x -= w // 2`; else `"right" in anchor` → `x -= w`.
  (Substring test, not equality.)
* `draw.text((x, y), text, font, fill=el.color)`.

Resolved for the shipped layout:

```
carrier "No Service" @14: w=91 → x = 120 - 45 = 75,  y = 51
carrier "Tello"      @14: w=44 → x = 120 - 22 = 98,  y = 51
clock   "17:08"      @14: w=44 → x = 213 - 44 = 169, y = 8
```

#### `icon_set` elements

Value selection:

* `prefix == "bat"` → `val = battery.level()` (0..4). If `battery.hardware` is false,
  set `bat_label = "?"` (and still use the simulated level).
* `prefix == "sig"` → `bars = modem.signal_level()`; `val = int(el.sim_val)` (default 3
  if the key is missing) when `bars is None`, else `bars`.
* otherwise → `val = int(el.sim_val)` (default 3).

Then `custom_path = el.custom_images.get(str(val))` — so an out-of-range `val` silently
falls back to the drawn-bars path.

**Custom-sprite path.** `img = _get_status_icon(custom_path)` = `get_image(path,
scale=H/240.0)` = `get_image(path, scale=0.7291666...)`. The scaler is
`w = max(1, int(img.width * scale))`, `h = max(1, int(img.height * scale))`, resized with
`Image.Resampling.LANCZOS` if the size actually changed, cached under the key
`"<path>@x0.729167"` (`%g` formatting of the float).

Source sprites are **36 × 180 palette PNGs with transparency** → scaled to **26 × 131**.
`canvas.paste(img, (x, y), img)` — alpha-composited onto the canvas.

Resolved: battery sprite at **(210, 17)**, signal sprite at **(7, 17)**, both 26×131.
Both therefore extend to y = 148, three rows past `content_bottom = 145`; the softkey bar
drawn afterwards covers those rows. Reproduce that overlap.

When `bat_label` is set, `vis_box = img.getbbox() or (0, 0, img.width, img.height)`.
Measured `getbbox()` of the **scaled** sprites (LANCZOS, RGBA, non-zero-pixel bounds):

| sprite | scaled size | getbbox |
| --- | --- | --- |
| bat-0 | 26×131 | (3, 91, 23, 127) |
| bat-1 | 26×131 | (3, 71, 23, 127) |
| bat-2 | 26×131 | (3, 50, 23, 127) |
| bat-3 | 26×131 | (3, 27, 23, 127) |
| bat-4 | 26×131 | (2, 2, 23, 127) |
| sig-0 | 26×131 | (0, 96, 26, 131) |
| sig-1 | 26×131 | (0, 71, 26, 131) |
| sig-2 | 26×131 | (0, 50, 26, 127) |
| sig-3 | 26×131 | (0, 27, 26, 127) |
| sig-4 | 26×131 | (0, 5, 26, 131) |

**Drawn-bars path** (no custom image for that value):

```
step = max(3, int(W * 0.021))              /* = max(3, 5) = 5 */
for i in 0 .. count-1:
    h     = (i + 1) * 3
    color = "white" if i <= val else "#333333"
    bx    = x + i * step
    draw.rectangle((bx, y + 15 - h, bx + 3, y + 15), fill=color)
vis_box = (0, 0, count * step, 15)         /* = (0, 0, 25, 15) */
```

Note `i <= val`, so `val = 3` lights **four** of five bars.

**`_draw_status_label(text, icon_x, icon_y, vis_box)`** — drawn whenever `bat_label` is
set, even if the sprite failed to load (then `vis_box = (0,0,12,15)`):

```
left, top, right, bottom = vis_box
tw, th = get_text_size(text, font_s)
tx = max(0, icon_x + left - tw - 4)
ty = icon_y + top + max(0, ((bottom - top) - th) // 2)
draw.text((tx, ty), text, font=font_s, fill="white")
```

Resolved for the no-fuel-gauge case at the simulated 3.85 V (level 3, bat-3, vis_box
`(3,27,23,127)`, `"?"` = 11×13): `tx = max(0, 210 + 3 - 11 - 4) = 198`,
`ty = 17 + 27 + ((127-27) - 13)//2 = 17 + 27 + 43 = 87`. **Verified: `(198, 87)`.**

#### `render_home()` — the whole frame

**Step 1, background.** Exactly one of:

* `self.wallpaper` is set → `canvas.paste(wallpaper, (0,0))` (already 240×175);
* else `home_layout["background"]` is truthy → lazily build `self._home_bg` once:
  `get_image(bg_path)`, `.convert("RGB").resize((240,175), LANCZOS)`, cache it on the
  instance, then `canvas.paste(bg, (0,0))`. (Shipped layout has `background: null`, so
  this branch is dormant.)
* else `draw.rectangle((0, 0, 240, 175), fill="black")`. Same rectangle when
  `home_layout` is `None` entirely.

**Step 2, elements.** Iterate `home_layout["elements"]` in array order. **Skip** any
element where `notify.active()` **and** `el.type == "text"` **and**
`el.text == "No Service"` — that is what frees the middle of the screen for the
notification banner. If `home_layout` is None, draw `"No Layout Found"` at `(10,10)` in
red with the **default** font (no `font=` argument is passed).

**Step 3, notification layer.**

```
if notify.active() or _unread_sms > 0:
    if int(time.time() * 2) % 2 == 0:                 /* 500 ms on, 500 ms off */
        env = _get_status_icon("/NeoDCT/System/ui/resources/img/envelope.png")
        if env:
            icon_scale = H / 240.0
            canvas.paste(env, (int(46*icon_scale) + 7, int(10*icon_scale)), env)

if notify.active():
    y = max(46, int(content_bottom * 0.34))           /* max(46, 49) = 49 */
    for line in notify.banner_lines():
        draw.text((30, y), line, font=font_n, fill="white")
        y += 24
```

Envelope source is 26×18 → scaled 18×13, pasted at **(40, 7)** (`int(46*0.729167)=33`,
`+7 = 40`; `int(10*0.729167) = 7`). `banner_lines()` returns
`("%d message[s]" % count, "received")` — two lines, so **(30, 49)** and **(30, 73)**.
Verified.

#### `render_home_dialing()`

```
if wallpaper: canvas.paste(wallpaper, (0,0))
else:         draw.rectangle((0, 0, 240, 175), fill="black")

if dial_buffer:
    w, h = get_text_size(dial_buffer, font_xl)
    y = max(50, int(content_bottom * 0.35))           /* max(50, 50) = 50 */
    draw.text(((240 - w)//2, y), dial_buffer, font=font_xl, fill="white")
```

Verified: `"0821234567"` (174 px wide at 24) → **(33, 50)**. Note this screen draws
**no** status icons, no clock and no carrier.

#### The softkey bar (`framework.py:447`, drawn by `NeoDCT_UI.update`)

`height = 30`, `y_start = 175 - 30 = 145`, `is_transparent = True` for the core's bar.

```
if is_transparent and ui.wallpaper:
    box = (0, 145, 240, 175)
    canvas.paste(wallpaper.crop(box), box)      /* on exception: black rectangle */
else:
    draw.rectangle((0, 145, 240, 175), fill="black")

if text:
    w, h = get_text_size(text, font_n)
    draw.text(((240 - w)//2, 145 + (30 - h)//2), text, font=font_n, fill="white")
```

For `font_n`, `h = 18` → text baseline row **151**. Verified: `"Menu"` at **(87, 151)**,
`"Call"` at **(97, 151)**, `"Read"` at **(90, 151)**.

#### `update()` — the per-frame dispatcher

```
HOME:         render_home();          softkey.update(notify.active() ? "Read" : "Menu",
                                                     present=False);  fb.update(canvas)
HOME_DIALING: render_home_dialing();  softkey.update("Call", present=False);
                                      fb.update(canvas)
MENU:         render_menu()           /* blocks; pushes its own frames */
```

`present=False` suppresses the softkey bar's own `fb.update`, so exactly **one**
framebuffer write happens per home frame.

---

### 9. Menu and app launch

`render_menu()` (`main.py:936`):

```
try:
    menu = AppSelector("Main Menu", self.apps, self, background=self.wallpaper)
    choice = menu.show()
    if choice != -1:
        print("[OS] Launching App ID: {choice}")     /* it is the index, not the manifest id */
        self.launch_app(self.apps[choice])
except (KeyboardInterrupt, IncomingCall):
    raise
except BaseException:
    print("[OS] Menu crashed"); traceback.print_exc(); log_crash("menu", sys.exc_info())
finally:
    self.state = "HOME"
```

`AppSelector` belongs to the UI-framework spec, but the core drives it, so its resolved
geometry at 240×175 is recorded here (measured, `framework.py:315`):

```
header_y      = max(30, int(175 * 0.11)) = 30
content_bottom = 145,  softkey_h = 30
background     = ui.wallpaper, or rectangle (0,0,240,175) black
app name       font_xl(24), centred, y = header_y - 16 = 14
icon_y         = 30 + max(24, int((145-30) * 0.22)) = 30 + 25 = 55
icon_cap       = min(APP_SELECTOR_ICON_MAX=175, max(24, 145 - 55 - 8)) = 82
icon           get_image(path, max_size=82); thumbnail() only if larger than 82
               pasted at ((240 - img.width)//2, 55) with its own alpha
missing icon   rectangle (px,55,px+82,55+82) outline white, px=(240-82)//2=79,
               plus "?" in font_xl centred in that box
"Select"       font_n(20), centred, y = 145 + (30-18)//2 = 151
scrollbar      line (232, 36, 232, 135) white width 2
notch          step = (135-36)/(n-1); notch_y = 36 + selected*step
               rectangle (228, notch_y-3, 234, notch_y+3) white
page number    str(selected+1) in font_n at (240 - 5 - w, 10)
empty list     "No Apps" in font_n, centred horizontally,
               y = max(30, 30 + ((145 - 30 - h)//2))
```

Verified first frame with 24 apps: title `"Phone book"` at (34,14), icon 82×82 at
(79,55), `"Select"` at (83,151), scrollbar line (232,36,232,135), notch rect
(228,33,234,39), page `"1"` at (225,10).

`AppSelector.show()` first **flushes evdev**: while `select([ui.keypad_fd],[],[],0.01)`
reports readable, `os.read(fd, 24)` and discard. Then draw, then loop on
`ui.wait_for_key()`: 108 = next (wraps), 103 = previous (wraps), 28 = return index,
14 = return −1. With an empty list only 14 and 28 are honoured, both returning −1.

`launch_app(app)` (`main.py:907`):

```
path = app.path + "/" + app.exec
spec = importlib.util.spec_from_file_location("neodct_app", path)
if spec is None or spec.loader is None: print("[OS] App load failed: {path}"); return
module = module_from_spec(spec); spec.loader.exec_module(module)
if hasattr(module, "run"): module.run(self)
else: print("[OS] App has no run(ui): {path}")

except (KeyboardInterrupt, IncomingCall): raise
except BaseException:
    print("[OS] App crashed: {name} ({path})"); traceback.print_exc()
    show_app_crash(self, app_name=name, exc_info=sys.exc_info())
finally:
    gc.collect()
    self._unread_sms = self._count_unread_sms()
```

Every app is imported under the **same module name `"neodct_app"`**, so each launch
replaces the previous module object.

**In C this becomes `fork()` + immediate `execve()` of `nd-apprun`, then `waitpid()`.**
The behaviours that must survive the translation:

* Normal return → back to `HOME`, no screen of any kind.
* Abnormal exit (any signal, or a non-zero status) → the equivalent of
  `show_app_crash(ui, app_name=..., exc_info=...)`: append a report to
  `/NeoDCT/User/logs/crash.log` (rotated at 64 KiB to `crash.log.1`), then, in
  engineering mode, flush input, draw `/NeoDCT/System/ui/resources/CRASH.jpg` resized to
  240×175 over the whole canvas plus a black strip `(0, 0, 240, th+4)` with a ≤90-char
  one-line summary at `(2,2)` in `font_s`, a `"Continue"` softkey (opaque, since an app
  bar), one `fb.update`, and wait for a key in `{14, 28, 46, 50, 96}`. Outside engineering
  mode: a plain `MessageDialog`. The signal number and faulting address replace the
  Python exception summary.
* `IncomingCall` must **not** be treated as a crash.
* After the app, always re-read the unread-SMS count from the database.

---

### 10. `read_keypress`, `wait_for_key`, `handle_input`

`read_keypress(timeout=0.1)` (`main.py:1191`) — **the chokepoint every screen in the OS
passes through**:

```
_battery_tick()
_modem_tick()
_ring_tick()          /* may raise IncomingCall */

if matrix_input:
    key = matrix_input.read_key(timeout)     /* burns up to `timeout` polling */
    if key is not None: return key
    /* falls through to evdev as well */

if keypad_fd is None:
    if matrix_input is None: sleep(max(0.0, timeout))   /* avoid a 100% CPU spin */
    return None

select([keypad_fd], [], [], timeout)     /* any exception -> return None */
if not readable: return None
data = os.read(keypad_fd, 24)            /* any exception -> return None */
if len(data) == 24: (sec, usec, type, code, value) = unpack("llHHI", data)
elif len(data) == 16: (sec, usec, type, code, value) = unpack("IIHHI", data)
else: return None
if type == 1 and value == 1: return code
return None
```

Points the C port must keep:

* **`struct input_event` is 16 bytes on the 32-bit ARM target** (two 32-bit `timeval`
  fields) and 24 on 64-bit hosts. Use `sizeof(struct input_event)` and read exactly one
  event; both size branches exist for exactly this reason.
* Only `EV_KEY` (`type == 1`) with `value == 1` (initial press) produces a key.
  **Autorepeat (`value == 2`) and release (`value == 0`) are ignored** — the phone has no
  key repeat anywhere.
* When **both** backends are live the idle cost is up to **2 × timeout** per call
  (matrix polls the full timeout, then `select` waits the full timeout again). That is
  the existing behaviour; reproduce it or raise it as a change.
* The three ticks run **before** any waiting, on every single call.

`wait_for_key()` is `while True: k = read_keypress(0.1); if k is not None: return k`.

`handle_input(code)` (`main.py:1240`) — the complete key table:

```
/* 1. Notification banner takes priority on HOME */
if state == "HOME" and notify.active():
    if code == 28: _open_notification(); return
    if code == 14: notify.dismiss();     return

if code == 28:                                   /* navi / enter */
    if state == "HOME":          state = "MENU"
    elif state == "HOME_DIALING":
        modem.dial(dial_buffer)
        dialer_ui.show_calling(self, dial_buffer)     /* blocks for the call */
        dial_buffer = ""; state = "HOME"

elif code == 14:                                 /* C / clear */
    if state == "HOME_DIALING":
        dial_buffer = dial_buffer[:-1]
        if not dial_buffer: state = "HOME"

elif code in (103, 108) and state == "HOME":     /* up / down */
    result = contact_manager.show_contact_selector(self, title="Select", btn_text="Call")
    if result:
        target, _ = result                       /* (id, name, number, speed_dial) */
        modem.dial(target[2])
        dialer_ui.show_calling(self, target[2], target[1])

elif code in DEV_KEYMAP and state in ("HOME", "HOME_DIALING"):
    char = DEV_KEYMAP[code]
    dial_buffer += char
    state = "HOME_DIALING"
    _play_dtmf(char)
```

Nothing else is handled — in particular **code 50 (`menu`) does nothing on the home
screen**, and `14` on `HOME` (without a notification) does nothing.

`_play_dtmf(char)`: map `"*" → "star"`, `"#" → "hash"`, otherwise the character itself;
if the result is all digits or is `"star"`/`"hash"`, call
`notify.play_tone("/NeoDCT/System/tones/dtmf/<name>.wav")` — a fire-and-forget
`aplay -q <path>` subprocess. So `-`, `.` and `,` are silent. The tone directory holds
`0.wav`–`9.wav`, `star.wav`, `hash.wav`.

`_open_notification()`:

```
kind   = notify.kind();  count = notify.count();  target = notify.latest_data()
notify.dismiss()
if kind != "sms": return
app  = first app whose name == "Messages", else None
path = app.path + "/" + app.exec  or  "/NeoDCT/System/apps/Messages/main.py"
import it as "neodct_app"
if count == 1 and target is not None: module.open_message(self, target)
else:                                 module.open_inbox(self)
/* KeyboardInterrupt re-raised; any other BaseException:
   "[NOTIFY] Read flow crashed" + traceback */
finally: _unread_sms = _count_unread_sms()
```

Note `dismiss()` happens **before** the app runs, and note this path does **not** call
`show_app_crash` — it only prints.

---

### 11. Ticks, modem events, database init

`_battery_tick()`: returns immediately if `_shutting_down`. Calls `battery.poll()`
inside try/except (`[BATT] Poll failed: <exc>`). If it returns the string `"shutdown"`,
call `_shutdown_low_battery()`. The 2.0 s rate limit lives inside `BatteryService`.

`_shutdown_low_battery()`:

```
_shutting_down = True
vcell = battery.vcell() or 0.0
print("[BATT] Battery empty (VCELL={vcell:.3f} V). Graceful shutdown.")
show_error(self, "Battery empty. Shutting down...", title="LOW BATTERY",
           button_text=None, wait_for_ack=False)      /* draw-only, no key wait */
sleep(3)
os.sync()
rc = os.system("poweroff")
if rc != 0:
    print("[BATT] poweroff failed (rc={rc}); resuming so dev sessions survive.")
    _shutting_down = False; return
while True: sleep(1)          /* freeze on the notice while init takes us down */
```

`show_pending_battery_warning()` — called from the main loop only, **never** from a tick,
so a modal cannot land mid-frame inside an app:

```
if state not in ("HOME", "HOME_DIALING"): return
warning = battery.take_pending_warning()          /* "low" | "critical" | None */
if warning is None: return
message = "BATTERY CRITICALLY LOW!" if warning == "critical" else "LOW BATTERY!"
print("[BATT] Warning: {message} (VCELL={vcell:.3f} V)")
show_error(self, message, title="Battery")        /* blocks for OK */
```

`_modem_tick()`: `modem.poll()` in try/except (`[MODEM] Poll failed: <exc>`, then
**return**). Then drain events:

```
while True:
    event = modem.take_pending_event()
    if event is None: break
    try:
        if not _handle_modem_event(event): break      /* port busy: requeued, retry next tick */
    except Exception as exc:
        print("[MODEM] Event {event[0]} failed: {exc}")
```

Note the failure case does **not** break the loop — it goes round again.

`_handle_modem_event((kind, data))`:

* `"sms_received"` → `status, record = modem.fetch_sms(data)`. `"busy"` →
  `modem.requeue_event(event)` and return `False`. `"ok"` → store the row, then
  `notify.post_sms(row_id)`, then `_unread_sms += 1`.
* `"sms_stored_check"` → `status, records = modem.read_stored_sms()`. `"busy"` → requeue,
  return `False`. Otherwise for each record `i`: store it, `notify.post_sms(row_id,
  tone=(i == 0))` — **one beep, not N** — and `_unread_sms += 1`.
* `"sms_sim"` → `data` is `(sender, body)`; store, `post_sms(row_id)`, `+= 1`.
* Call events (`incoming`, `connected`, `ended`, …) fall through and return `True`
  unhandled.

`_store_incoming_sms(sender, body)` opens `/NeoDCT/User/db/sms_inbox.db`, re-runs the
`CREATE TABLE IF NOT EXISTS inbox` statement, inserts
`(message=body, sender=sender, timestamp=int(time.time()), is_read=0)`, commits, closes,
prints `[NOTIFY] SMS stored (id <row_id>) from <sender>`, returns the row id.

`_count_unread_sms()` returns `SELECT COUNT(*) FROM inbox WHERE is_read = 0` from the same
database, or `0` on any exception.

`init_databases()` (`main.py:441`) — run before anything else in the constructor:

* `mkdir /NeoDCT/User/db` if missing, logging
  `[CORE] Creating User DB directory: /NeoDCT/User/db`.
* `phonebook.db`: `PRAGMA journal_mode=WAL`, then
  `CREATE TABLE IF NOT EXISTS contacts (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT,
  number TEXT, speed_dial INTEGER)`. If `SELECT count(*) FROM contacts` is 0, log
  `[CORE] Seeding default contacts...` and insert `("NeoDCT Support", "555-1234", 2)`,
  then commit. **The contacts table creation is never committed explicitly** — it relies
  on sqlite's implicit DDL commit; keep the same statement order.
* `sms_inbox.db`: WAL, then `CREATE TABLE IF NOT EXISTS inbox (id INTEGER PRIMARY KEY
  AUTOINCREMENT, message TEXT, sender TEXT, timestamp INTEGER, is_read INTEGER DEFAULT 0)`,
  commit, close.
* `sms_outbox.db`: WAL, then `CREATE TABLE IF NOT EXISTS outbox (id INTEGER PRIMARY KEY
  AUTOINCREMENT, message TEXT, timestamp INTEGER)`, commit, close.
* `print("[CORE] Databases initialized successfully.")`

WAL is deliberate power-loss hardening and every later connection inherits it.

---

### 12. Ringing, `IncomingCall`, and the main loop

`_ring_tick()` (`main.py:1055`):

```
if _handling_call: return                       /* the ring UI is already up */
if modem.state != "RINGING": _ring_seen_at = None; return
if _ring_seen_at is None: _ring_seen_at = monotonic()
/* +CLIP arrives a beat after RING; give it one poll cycle so the screen
   opens with the caller's name rather than "Unknown" */
if modem.caller_id is None and (monotonic() - _ring_seen_at) < 0.6: return
raise IncomingCall(modem.caller_id)
```

`class IncomingCall(BaseException)` carries `.number`. It derives from `BaseException`
**on purpose**, exactly like `KeyboardInterrupt`, so that an app's `except Exception:`
cannot swallow a ringing phone. Unwinding runs every `finally:` on the way out, which is
how MusicPlayer and Koki release ALSA before the ringtone starts.

`run(fb)`'s loop:

```
while True:
    try:
        ui.update()
        ui.show_pending_battery_warning()
        key = ui.read_keypress(0.1)
        if key is not None:
            print("[INPUT] Code: {key}")
            ui.handle_input(key)
    except IncomingCall as call:
        ui.handle_incoming_call(call.number)
    except KeyboardInterrupt:
        raise
    except BaseException:
        print("[CORE] Unhandled exception in main loop")
        traceback.print_exc()
        log_crash("core-main-loop", sys.exc_info())
        sleep(0.1)
```

The whole body is inside the `try`, so an `IncomingCall` raised from *anywhere* — including
from deep inside an app, a menu or a modal dialog — lands here.

`handle_incoming_call(number)`:

```
_handling_call = True;  state = "HOME";  name = None
print("[CORE] Incoming call from {number or 'unknown'}")
notify.start_ring()
result = incoming_ui.show_incoming(self, number)      /* "answered" | "declined" | "gone" */
notify.stop_ring()

if result == "answered":
    if modem.answer():
        dialer_ui.show_calling(self, number or "", name)
        modem.hangup()                 /* make sure the line is really down */
    else:
        print("[CORE] Answer failed; releasing the call."); modem.hangup()
elif result == "declined":
    print("[CORE] Call declined."); modem.hangup()
else:
    print("[CORE] Caller hung up before we answered.")

except KeyboardInterrupt: raise
except BaseException:
    print("[CORE] Incoming call flow crashed"); traceback.print_exc()
    try: modem.hangup()
    except Exception: pass
finally:
    notify.stop_ring();  _handling_call = False;  state = "HOME"
```

`name` is always `None` here — the caller-name lookup lives inside
`incoming_screen._lookup_contact_name`, not in the core.

**The C translation.** Apps are separate processes, so there is no stack to unwind
across:

1. The modem thread in the core sees `RINGING` and applies the same 0.6 s `+CLIP` grace.
2. If an app child is running, the core sends it `SIGTERM`; the child's handler runs the
   app's teardown (the ALSA release that Python's `finally` did) and `_exit()`s.
3. The core `waitpid()`s, then runs `handle_incoming_call()` exactly as above.
4. If no app is running, the core raises the same flag out of its own `read_keypress`
   equivalent and jumps straight to step 3.

The `_handling_call` re-entrancy guard, the `_ring_seen_at` grace window, and the
"always `stop_ring()` and return to `HOME`" cleanup are all mandatory.

---

### 13. Image cache and wallpaper

`get_image(path, max_size=None, scale=None)` (`main.py:714`):

* **Path fixup**: if `path` starts with `"/home"` and contains `"System"`, rewrite it as
  `"/NeoDCT" + path.split("NeoDCT")[-1]`. Otherwise use it unchanged. (A development
  left-over that must be ported — some manifests could still carry such paths.)
* Cache key: `"<clean>@<int(max_size)>"` if `max_size`, else `"<clean>@x<%g of scale>"`
  if `scale`, else the plain path.
* Miss → `Image.open(clean).convert("RGBA")`, then:
  * `max_size` and (`width > max_size` or `height > max_size`) →
    `img.thumbnail((max_size, max_size), LANCZOS)` (aspect-preserving, never upscales);
  * elif `scale` → `w = max(1, int(width*scale))`, `h = max(1, int(height*scale))`,
    resize with LANCZOS only if the size changed.
* `_cache_put`: **FIFO cap of 32 entries** — when full, evict the oldest inserted key
  (`image_cache.pop(next(iter(...)))`, relying on dict insertion order).
* Any exception anywhere → return `None`.

The cache is **entry-counted, not byte-budgeted**. The C port should keep the same
32-entry FIFO so eviction order is identical, but a byte cap is worth raising as a
question (`CODING-STANDARDS.md` §4 wants byte budgets).

`load_wallpaper(path)` (`main.py:683`):

```
if not exists(path): print("[UI] No wallpaper found."); return None
print("[UI] Loading wallpaper: {path}")
ImageFile.LOAD_TRUNCATED_IMAGES = True        /* also set globally at import */
img = Image.open(path); img.load()
img = img.convert("RGB").resize((240, 175), LANCZOS)
img = ImageEnhance.Brightness(img).enhance(0.3)    /* dim to 30% */
return img
/* on exception: print("[UI] Wallpaper load error: {e}"); return None */
```

`ImageEnhance.Brightness(img).enhance(0.3)` is `Image.blend(black_image, img, 0.3)`, i.e.
per channel `out = round(0 + 0.3 * (v - 0))` with Pillow's blend rounding
(`(int)(a + f*(b-a) + 0.5)` on unsigned bytes). Reproduce that rounding exactly or the
wallpaper differs by one level in places.

`load_layout(path)` returns parsed JSON, or `None` on any exception.

`ImageFile.LOAD_TRUNCATED_IMAGES = True` is set at module import — a JPEG missing its EOI
marker must still decode as far as it got, not fail.

---

### 14. Memory footprint of this subsystem in C

Write the arithmetic next to each allocation (`CODING-STANDARDS.md` §4):

| Buffer | Bytes | Notes |
| --- | --- | --- |
| framebuffer mmap | 240 × 175 × 4 = **168,000** | shared with the kernel, not process-private dirty memory |
| `canvas` RGB888 | 240 × 175 × 3 = **126,000** | the one long-lived UI surface |
| BGRA staging for a frame | 240 × 175 × 4 = **168,000** | can be eliminated: pack straight into the mmap |
| `native_img` (slow path only) | stride_pixels × yres × 3 | never allocated on the 32bpp path |
| `_rgb565_out` (slow path only) | line_length × yres | never allocated on the 32bpp path |
| `_rgb565_band_out` (slow path only) | xres × yres × 2 | never allocated on the 32bpp path |
| wallpaper, if set | 240 × 175 × 3 = **126,000** | |
| `_home_bg`, if the layout has one | 240 × 175 × 3 = **126,000** | dormant with the shipped layout |
| status sprites | 2 × 26 × 131 × 4 = **27,248** | plus the envelope, 18 × 13 × 4 = 936 |
| image cache | up to 32 entries, **unbounded bytes** | see Risks |
| app icon in the selector | 82 × 82 × 4 = **26,896** each | 24 apps × 26,896 = 645 KB if all are visited |

The 32bpp fast path should pack directly from the RGB canvas into the mmap, removing the
168 KB staging buffer that Pillow's `tobytes()` forces.

---

## Public interface (the functions other parts call)

Every app and every widget receives the `ui` object and uses a stable subset of it. This
is the contract `libneodct.so` must expose. Usage counts are `grep`-verified across the
overlay: `framework.py` alone touches it 142 times, MusicPlayer and Messages 30 each.

### Data members apps read

| Python | C equivalent | Value |
| --- | --- | --- |
| `ui.W` | `nd_ui.w` | 240 |
| `ui.H` | `nd_ui.h` | 175 |
| `ui.SOFTKEY_H` | `nd_ui.softkey_h` | 30 |
| `ui.content_bottom` | `nd_ui.content_bottom` | 145 |
| `ui.canvas` | `nd_image *canvas` | 240×175 RGB888 |
| `ui.draw` | drawing context bound to the canvas | |
| `ui.fb` | `nd_fb *` | |
| `ui.font_s` / `font_md` / `font_n` / `font_xl` | `nd_font *` | 14 / 18 / 20 / 24 |
| `ui.state` | `nd_ui_state` | `HOME`, `HOME_DIALING`, `MENU` |
| `ui.apps` | `nd_app_entry[]` | name, icon, path, exec, id |
| `ui.wallpaper` | `nd_image *` or NULL | 240×175 RGB, dimmed to 30% |
| `ui.softkey` | `nd_softkeybar` | the system (transparent) bar |
| `ui.image_cache` | `nd_imgcache` | 32-entry FIFO |
| `ui.keypad_fd` | `int` | −1 when absent; widgets read it **directly** to flush input |
| `ui.matrix_input` | opaque handle or NULL | `framework._t9_active()` keys T9 off its presence |
| `ui.engineering_mode` | `bool` | |
| `ui.modem` / `ui.battery` / `ui.notify` | service handles | |
| `ui.dial_buffer` | string | |

### Methods

```c
/* Measurement — the primitive every centring calculation uses. */
void   nd_ui_text_size(nd_ui *ui, const char *text, nd_font *f, int *w, int *h);

/* Image loading, cached. Exactly one of max_size / scale may be given. */
nd_image *nd_ui_get_image(nd_ui *ui, const char *path);
nd_image *nd_ui_get_image_max(nd_ui *ui, const char *path, int32_t max_size);
nd_image *nd_ui_get_image_scaled(nd_ui *ui, const char *path, double scale);

/* Input. Returns a Linux keycode, or -1 for "nothing".
   Runs the battery/modem/ring ticks first, every call.
   Sets *ring_number and returns ND_KEY_INCOMING_CALL where Python raised. */
int32_t nd_ui_read_keypress(nd_ui *ui, double timeout);
int32_t nd_ui_wait_for_key(nd_ui *ui);

/* Frame presentation. */
nd_err nd_fb_update(nd_fb *fb, const nd_image *img);

/* Screens owned by the core. */
void nd_ui_render_home(nd_ui *ui);
void nd_ui_render_home_dialing(nd_ui *ui);
void nd_ui_render_menu(nd_ui *ui);          /* blocks */
void nd_ui_update(nd_ui *ui);               /* the per-frame dispatcher */
void nd_ui_handle_input(nd_ui *ui, int32_t code);
void nd_ui_show_pending_battery_warning(nd_ui *ui);
void nd_ui_handle_incoming_call(nd_ui *ui, const char *number);
void nd_ui_launch_app(nd_ui *ui, const nd_app_entry *app);

/* Framebuffer lifecycle. */
nd_err nd_fb_open(nd_fb **out, const char *path);
void   nd_fb_close(nd_fb *fb);

/* Input backends. */
nd_err  nd_input_discover_evdev(char *out, size_t out_sz);
nd_err  nd_keymap_load(const char *path, nd_keymap *out);   /* ND_ERR_NOTFOUND == "no matrix" */
int32_t nd_input_matrix_read_key(nd_matrix_input *in, double timeout);

/* Database bootstrap. */
nd_err nd_db_init(void);
```

`nd_ui_read_keypress` replacing an exception with a sentinel is the one interface change.
Because apps are separate processes, the sentinel only has to exist inside the core; apps
receive `SIGTERM` instead.

---

## External dependencies and their C replacements

| Dependency | Used for | C replacement |
| --- | --- | --- |
| `PIL.Image.new` | the 240×175 canvas, `native_img`, splash canvas | `nd_image_new()` |
| `PIL.Image.open` | wallpaper, status sprites, app icons, CRASH.jpg | `nd_image_load_png()` / `nd_image_load_jpeg()` (libpng + libjpeg, already in the image) |
| `PIL.Image.convert("RGB"/"RGBA")` | palette→RGBA sprites, canvas normalisation | `nd_image_convert()` |
| `PIL.Image.resize(..., LANCZOS)` | wallpaper 240×175, sprite ×0.729167, `_home_bg` | `nd_image_resize_lanczos()` — **must match Pillow's 3-lobe Lanczos and its support/clipping** |
| `PIL.Image.thumbnail(..., LANCZOS)` | app icons capped at 82 px | `nd_image_thumbnail()` — aspect-preserving, never upscales |
| `PIL.Image.crop` | fb source centring, softkey wallpaper strip | `nd_image_crop()` |
| `PIL.Image.paste(img, xy[, mask])` | every sprite and background | `nd_image_blit()` / `nd_image_blit_alpha()` |
| `PIL.Image.paste(colour, box)` | clearing `native_img` | `nd_image_fill_rect()` |
| `PIL.Image.getbbox()` | `vis_box` for the battery `?` label | `nd_image_alpha_bbox()` — non-zero-pixel bounds over **all** channels |
| `PIL.Image.tobytes("raw","BGRA"/"BGR;16")` | framebuffer packing | `nd_fb_pack_bgra()` / `nd_fb_pack_rgb565()` |
| `PIL.ImageDraw.text` | all text | `nd_draw_text()` (FreeType, anchor `la`) |
| `PIL.ImageDraw.rectangle` | backgrounds, softkey strip, bars | `nd_draw_rect()` — Pillow rectangles are **inclusive** of both corners |
| `PIL.ImageDraw.line` | scrollbar (AppSelector) | `nd_draw_line()` |
| `PIL.ImageDraw.textbbox` | `get_text_size` | `nd_text_bbox()` |
| `PIL.ImageFont.truetype` | font.ttf at 14/18/20/24, DejaVu at 14/20 | FreeType, or baked bitmap tables |
| `PIL.ImageFont.load_default` | font-load failure path | a tiny built-in 6×11 fallback (see Open Questions) |
| `PIL.ImageEnhance.Brightness(img).enhance(0.3)` | wallpaper dimming | `nd_image_scale_brightness()` with Pillow's `int(a + f*(b-a) + 0.5)` rounding |
| `PIL.ImageFile.LOAD_TRUNCATED_IMAGES` | JPEGs without EOI | libjpeg error manager that returns what it decoded |
| `mmap` | framebuffer mapping | `mmap(2)` |
| `fcntl.ioctl` | `FBIOGET_VSCREENINFO 0x4600`, `FBIOGET_FSCREENINFO 0x4602`, `I2C_SLAVE 0x0703` | `ioctl(2)` with `<linux/fb.h>` / `<linux/i2c-dev.h>` |
| `struct.unpack` | fb info, `input_event` | real structs; **no unaligned access on ARM** |
| `select.select` | evdev wait, input flush | `poll(2)` |
| `os.open/read/write/close` | evdev, i2c, framebuffer | direct syscalls |
| `glob.glob` | `/dev/input/by-path/*-kbd`, `by-id/*-kbd`, `event*`, `/dev/gpiochip*` | `opendir` + `fnmatch`, then `qsort` with `strcmp` to match Python's `sorted()` |
| `os.path.realpath` | resolving input symlinks | `realpath(3)` |
| `json` | `ui_home.json`, `manifest.json`, `keymap.json` | a small JSON reader in `libneodct.so` (no cJSON dependency needed for these shapes) |
| `sqlite3` | phonebook, sms inbox/outbox | `libsqlite3` (already in the image, ~2.3 MB of the Python figure is the *binding*, not the library) |
| `importlib.util` | loading `main.py` | `fork()` + `execve("nd-apprun")`; `nd-apprun` does the `dlopen` |
| `gpiozero` (`Button`, `OutputDevice`) | GPIO matrix keypad | `libgpiod` v2, or `/dev/gpiochipN` `GPIO_V2_LINE` ioctls directly. `Button(pull_up=True)` = input with pull-up, pressed == line low; `OutputDevice(initial_value=True)` = output driven high |
| `subprocess.Popen(["aplay", ...])` | DTMF and SMS tones | `posix_spawn` of `/usr/bin/aplay` (owned by the notify agent) |
| `traceback` | crash reports | `backtrace()` / signal + fault address |
| `gc.collect()` | after every app | `malloc_trim(0)`, or nothing — with process-per-app the memory is already gone |
| `os.system("poweroff")` | low-battery shutdown | `posix_spawn("/sbin/poweroff")`, keeping the non-zero-rc "resume so dev sessions survive" path |
| `os.sync()` | before poweroff | `sync(2)` |
| `os.execv(sys.executable, argv)` | keypad wizard restart | `execv("/proc/self/exe", argv)` |
| `time.monotonic` | ring grace, key timeouts | `clock_gettime(CLOCK_MONOTONIC)` |
| `time.time` | timestamps, envelope blink | `clock_gettime(CLOCK_REALTIME)` |
| `time.strftime("%H:%M")` | the home clock | `strftime` with `localtime_r` |
| `print` → serial | all logging | `nd_log(ND_LOG_FB/INPUT/UI/CORE/OS/MODEM/BATT/NOTIFY, ...)` |

---

## Proposed C modules

| File | Contents | Est. LOC |
| --- | --- | --- |
| `nd_fb.c` / `nd_fb.h` | `nd_fb_open` (two ioctls, mmap, zero-fill, pixel-path detection and its log line), `nd_fb_pack_bgra`, `nd_fb_pack_rgb565` (with the 3 × 256 lookup tables), `nd_fb_write_center_band`, `nd_fb_update`, `nd_fb_close` | 330 |
| `nd_input.c` / `nd_input.h` | `_event_device_name`, `_discover_keypad_path`, `_gpio_available`, evdev open with the `KEYPAD_PATH` retry, the `input_event` read and the `EV_KEY`/`value==1` filter, the input flush helper | 320 |
| `nd_keymap.c` / `nd_keymap.h` | `MATRIX_NAME_TO_CODE`, `nd_keymap_load` with every rejection message | 210 |
| `nd_input_gpio.c` | `MatrixKeypadInput` over libgpiod: row drive, 1 ms settle, full-matrix scan, `_held`/`_pending` rollover, `_last_unmapped` logging, `read_key` with the 5 ms poll | 260 |
| `nd_ui.c` / `nd_ui.h` | `nd_ui` struct, construction in the exact order of §6, `nd_ui_update`, `_play_dtmf`, `_count_unread_sms`, `_open_notification`, `_shutdown_low_battery`, `show_pending_battery_warning`, teardown | 480 |
| `nd_ui_home.c` | `render_element` (both element types, both icon paths), `_get_status_icon`, `_draw_status_label`, `render_home`, `render_home_dialing` | 380 |
| `nd_ui_input.c` | `nd_ui_read_keypress` (tick order, matrix-then-evdev, the no-backend sleep), `nd_ui_wait_for_key`, `nd_ui_handle_input` with the full key table and `DEV_KEYMAP` | 260 |
| `nd_ui_ticks.c` | `_battery_tick`, `_modem_tick`, `_handle_modem_event`, `_store_incoming_sms`, `_ring_tick`, `handle_incoming_call` | 340 |
| `nd_layout.c` / `nd_layout.h` | `ui_home.json` parse into a typed element array; `load_layout` failure semantics | 200 |
| `nd_applist.c` | `_scan_apps_from_dir`, manifest parse with all five defaults, sort by `id` | 170 |
| `nd_applaunch.c` | `fork()`+`execve("nd-apprun")`, `waitpid`, exit-status classification, crash-screen invocation, post-app `_unread_sms` refresh | 260 |
| `nd_imgcache.c` / `nd_imgcache.h` | `get_image` with the `/home` path fixup, the three key forms, `max_size`/`scale` behaviour, the 32-entry FIFO | 230 |
| `nd_dbinit.c` | `init_databases` — three databases, WAL, the schemas, the seed contact | 150 |
| `nd_splash.c` | `show_boot_logo` and `splash_version` | 110 |
| `nd_main.c` | `launcher.main()`: serial redirect + log colour install, `ClockService.start`, `RemoteShell.start_if_enabled`, framebuffer, splash, 1 s sleep, `nd_core_run` | 170 |
| `nd_core_loop.c` | `run(fb)`: first-time keypad setup hook, `nd_ui` construction, the main loop with its three catch arms and `log_crash("core-main-loop", …)` | 190 |
| **Total** | | **≈ 4,060** |

Rounding for the estimate below: **≈ 3,900 LOC** of `.c` plus headers, against 1,437 LOC
of Python. The ratio is high because Pillow calls become explicit loops and because every
Python `try/except` becomes a checked return.

---

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| **Text metrics drift.** Advances are fractional (`n × size / 8`), and every centring calculation in the OS depends on `ceil(Σ advance)`. Rounding per glyph instead of per string moves text by 1–3 px on most strings. | **high** | Implement `nd_text_size` as fixed-point accumulation then a single `ceil`. Unit-test against the measured table in §7 (`"Tello"`→44, `"No Service"`→91, `"Menu"`→65, `"0821234567"`→174) before any screen is drawn. |
| **LANCZOS resampling mismatch.** The 36×180 → 26×131 sprite downscale, the wallpaper 240×175 resize, and the 82 px icon thumbnails all use Pillow's Lanczos. A different filter changes every anti-aliased edge, and it changes `getbbox()` — which moves the battery `"?"` label. | **high** | Port Pillow's `resample.c` Lanczos (3-lobe, `support = 3.0`, its exact coefficient normalisation and clamping) rather than writing a fresh one. Golden-frame the ten status sprites first — they are the cheapest possible regression test. |
| **`fb_fix_screeninfo` offset.** The Python reads `line_length` at byte 48, which is the 64-bit offset; on 32-bit ARM that is `mmio_start`. It only works because the driver reports 0 and the fallback fires. | medium | Use `struct fb_fix_screeninfo` from `<linux/fb.h>` **and keep** the `== 0 → xres*bpp/8` fallback, so the value on hardware is unchanged. Logged in Open Questions. |
| **Image cache is entry-counted, not byte-budgeted.** 32 entries of 82×82 RGBA app icons is 860 KB; nothing stops a single huge PNG from occupying one slot. | medium | Keep the 32-entry FIFO so eviction order matches, and add a byte ceiling that only ever evicts *earlier* than Python would. Raise as a question before changing eviction order. |
| **`IncomingCall` semantics.** In Python the unwind runs each app's `finally:`, which is what releases ALSA before the ringtone. A `SIGTERM` whose handler does not run the same teardown will leave the sound card busy and the ringtone silent. | **high** | `nd-apprun` installs a `SIGTERM` handler that calls the app's `app_shutdown()` (mandatory in the app ABI) then `_exit(0)`; the core waits with a timeout and escalates to `SIGKILL`. Specify the teardown contract in the app-ABI spec. |
| **Koki never calls `read_keypress`,** so today a call does not interrupt it. `ARCHITECTURE.md` proposes fixing this with a modem thread. | medium | Already in `OPEN-QUESTIONS.md`. Whichever answer comes back, the core must handle both: the flag path (app cooperates) and the signal path. |
| **`get_setting` rewrites `settings.prop` on almost every call** (`load_with_defaults` saves whenever any default is missing or any `system.os.*` key is stale). The constructor calls it twice and the splash once. On NAND that is avoidable write wear. | medium | Reproduce the read semantics exactly; cache the parsed file in `libneodct.so` for the process lifetime and re-check `st_mtime`. Do not change *when* the file is rewritten without asking. |
| **Double timeout when both input backends are live** — up to 200 ms per idle poll. | low | Reproduce as written, or unify both backends behind one `poll()` with a shared deadline. Flag before changing. |
| **`gpiozero` has no C equivalent in the image.** libgpiod is not currently a Buildroot selection. | medium | Either add `BR2_PACKAGE_LIBGPIOD` or talk to `/dev/gpiochipN` with `GPIO_V2_GET_LINE_IOCTL` directly (~120 LOC, no new package). The PCF8575 backend — the one real hardware actually uses — needs neither. |
| **`ImageFont.load_default()` fallback.** If `font.ttf` fails to load, Pillow substitutes a specific built-in bitmap font and the entire UI still renders (badly). There is no C equivalent. | low | Ship a minimal built-in fallback face, or treat a font-load failure as fatal with a serial message. Question raised. |
| **Blocking calls inside the render path.** `render_menu` (`AppSelector.show`), `show_calling`, `show_incoming`, `show_contact_selector` and `MessageDialog.show` all block inside what looks like a per-frame function. | low | Keep the same structure — the phone is deliberately single-tasking — but make sure the modem thread and its ring detection are outside it. |
| **16bpp slow path.** If `neodct_displayd` loses the race, the framebuffer stays 16bpp and Python needs ~350 ms/frame. C makes this cheap, so the warning stops mattering. | low | Keep the log lines identical so existing serial-log scraping still works, but the C 16bpp packer is a plain loop — no need for the lookup tables. |
| **`show_alpha_security_notice_once` runs mid-construction,** before `engineering_mode`, `home_layout`, `wallpaper` and `apps` exist. Any C code that reads those from the dialog path will read uninitialised memory. | medium | Zero-initialise the whole `nd_ui` struct before step 1 and keep the construction order literally as in §6. |
| **`getbbox()` semantics.** Pillow's `getbbox()` on RGBA treats a pixel as empty only when **all four** channels are zero — an opaque black pixel counts as ink. | low | Implement `nd_image_alpha_bbox` on that rule, not on alpha alone. Test against the ten measured sprite bboxes in §8. |

### Open questions raised by this survey

These are written in `OPEN-QUESTIONS.md` format so they can be copied across verbatim.
This survey did not edit that file (parallel agents are writing it).

#### core/fb — reproduce the `line_length` offset bug, or read the real struct?
**Found in:** `System/core/main.py:301`
**What the Python does:** `struct.unpack_from("I", finfo, 48)` — offset 48 is
`line_length` on 64-bit but `mmio_start` on the 32-bit ARM target, where the following
`if line_length == 0` fallback rescues it with `xres * bpp/8`.
**Why it is unclear:** "Port the bug too" would mean reading the wrong field. Reading
`finfo.line_length` properly is a silent behaviour change if any driver ever pads rows.
**Options:** (a) read the real field and keep the `== 0` fallback (proposed);
(b) reproduce the offset literally.
**Answer:** _(pending)_

#### core — is the up-to-200 ms idle poll when both input backends are live intended?
**Found in:** `System/core/main.py:1197-1216`
**What the Python does:** polls the matrix for the whole timeout, then `select()`s evdev
for the whole timeout again.
**Why it is unclear:** on hardware only one backend is normally live, so it may never
have been noticed. It halves the effective frame rate when both are.
**Options:** (a) reproduce exactly; (b) share one deadline across both backends.
**Answer:** _(pending)_

#### core — what should replace `ImageFont.load_default()` when `font.ttf` fails?
**Found in:** `System/core/main.py:611-616`
**What the Python does:** falls back to Pillow's built-in bitmap font; the UI stays
usable but every layout shifts.
**Why it is unclear:** there is no C equivalent and the fallback frames are not in any
golden set.
**Options:** (a) ship a small built-in face; (b) treat a font-load failure as fatal with
a serial message.
**Answer:** _(pending)_

#### core — may the 32-entry image cache gain a byte ceiling?
**Found in:** `System/core/main.py:712, 754-758`
**What the Python does:** FIFO eviction at 32 entries, no byte budget.
**Why it is unclear:** `CODING-STANDARDS.md` §4 requires byte-budgeted caches, but a byte
cap changes *which* entries survive and therefore which frames re-decode.
**Options:** (a) keep 32-entry FIFO exactly; (b) 32 entries **and** a byte ceiling that
can only evict earlier than Python would.
**Answer:** _(pending)_

#### core — should `get_setting()` keep rewriting `settings.prop` on every read?
**Found in:** `System/core/SettingsStorage/__init__.py:95-116`, called from
`System/core/main.py:623, 636` and `launcher.py:45`
**What the Python does:** `load_with_defaults()` re-saves the whole file whenever a
default is missing or a `system.os.*` key is stale — so many "reads" are also writes.
**Why it is unclear:** it is NAND write wear on a phone that boots read-only, but it is
also the mechanism that materialises defaults on first boot.
**Options:** (a) reproduce exactly; (b) cache in-process and re-check `st_mtime`, writing
only when the content would actually change.
**Answer:** _(pending)_

---

## Tests that cover this

`neodct/tests/` has no test that imports `System.core.main` directly. Coverage of this
subsystem is entirely through **`neodct/tests/test_uistub.py`** (48 test functions), which
boots the *real* `NeoDCT_UI` against `neodct/tools/uistub.py`'s stubbed hardware. That is
the port oracle, and it is a good one.

What it already pins down:

| Test | What it fixes |
| --- | --- |
| `test_device_frame_centres_ui_band_on_240x240_panel` | the 240×175 band letterboxed on a 240×240 panel |
| `test_device_frame_band_starts_at_row_32` | `dst_y = (240-175)//2 = 32` |
| `test_framebuffer_captures_frame_independent_of_later_canvas_edits` | the canvas is long-lived and reused; a frame must be copied, not referenced |
| `test_stub_ui_scans_the_shipped_app_manifests` | `_scan_apps_from_dir` finds Phone book / Messages / Games |
| `test_stub_ui_sorts_apps_by_manifest_id` | the `sort(key=id)` and that Phone book is id 1 |
| `test_engineering_apps_are_hidden_when_engineering_mode_is_off` | the `engineering/apps` directory is conditional |
| `test_stub_ui_renders_a_240x175_home_frame` | `update()` on `HOME` emits exactly one 240×175 frame |
| `test_stub_ui_loads_the_shipped_pixel_font` | `font_xl.size == 24` (i.e. `font.ttf` loaded, not the default) |
| `test_stub_ui_applies_a_wallpaper_setting` | wallpaper resolves through `system.ui.wallpaper` and lands at 240×175 |
| `test_stub_ui_without_wallpaper_renders_a_black_home_background` | the `wallpaper is None` branch |
| `test_scripted_keys_drive_the_app_selector_to_a_choice` / `..._back_key_returns_minus_one` | key 108 → next, 28 → index, 14 → −1 |
| `test_stub_ui_never_attaches_to_a_real_input_device` | the `keypad_fd is None` "no input backend" path |
| `test_run_app_*` (6 tests) | `launch_app`'s importlib-then-`run(ui)` contract, including apps that never return |
| `test_simulate_status_*` | `battery.hardware`/`level()` and `modem.signal_level()`/`operator_display()` are exactly the four accessors the home screen reads |

**Using it as the port oracle.** `CapturingFramebuffer` records the band *before* it is
packed for the framebuffer, so a Python-vs-C diff at 240×175 RGB isolates rendering bugs
from pixel-packing bugs. Capture golden frames for, at minimum:

1. `HOME` with no hardware — `"No Service"`, battery `"?"` at (198,87), the `#333333`
   fallback bars never appearing (custom sprites exist for 0–4).
2. `HOME` with `simulate_status(battery=4, signal=3, carrier="Tello")`.
3. `HOME` for every battery level 0–4 and every signal level 0–4 (the ten sprite bboxes).
4. `HOME` with a notification active, both blink phases (`int(time()*2) % 2`).
5. `HOME` with a wallpaper set — this is the only frame that exercises the transparent
   softkey bar's wallpaper crop.
6. `HOME_DIALING` with a 1-, 5- and 10-digit buffer.
7. The `AppSelector` first frame and the frame after one `108`.
8. The boot splash (`show_boot_logo`) — note it needs DejaVu, so it is the one frame
   whose golden depends on the target's font package.

The clock (`strftime("%H:%M")`) and the envelope blink make frames time-dependent; freeze
both when capturing (the layout substitution is keyed on the literal string `"12:00"`, so
a harness can pin it).

Tests that exist and are **not** relevant here: everything under `test_update_*`,
`test_initramfs_*`, `test_t9_*`, `test_storage`, `test_clockservice`, `test_remoteshell`.

---

## How this could be split across agents

This subsystem is the foundation everything else is verified against, so the first two
work packages are strictly sequential and everything after them can fan out.

**Wave 0 — the oracle (blocking, 1 agent, no C).** Capture the golden frames listed above
from the Python build via `uistub.py`, and write the comparison harness. Nothing else
starts until this exists, because without it "one-to-one" is unmeasurable.

**Wave 1 — pixels (blocking, 2 agents in parallel).**

* *Agent A — rasterizer and font.* `nd_image`, `nd_draw`, Lanczos resampling,
  `alpha_bbox`, brightness scaling, and `nd_text_size`/`nd_draw_text`. Verified against
  the measured metric table in §7 and the ten sprite bboxes in §8. This is the highest-risk
  work in the whole port and it belongs to one agent, not several.
* *Agent B — `nd_fb.c`.* Purely mechanical and completely independent of the rasterizer:
  two ioctls, a mmap, two packers and the band write. Testable against a `/dev/fb0`
  emulated by a file plus a fake-ioctl shim, with no drawing involved.

**Wave 2 — four independent packages (4 agents in parallel), each only needing Wave 1.**

* *Input.* `nd_input.c`, `nd_keymap.c`, `nd_input_gpio.c`. No rendering at all; testable
  against synthetic `/dev/input` nodes and a fabricated `keymap.json`. Can actually start
  during Wave 1.
* *Home screen.* `nd_layout.c`, `nd_ui_home.c`, `nd_imgcache.c`. The single largest
  pixel-exactness job; measured against goldens 1–6.
* *Services glue.* `nd_ui_ticks.c` and `nd_dbinit.c`. Depends on the modem, battery and
  notify specs for their accessor signatures, but nothing else here; testable with fake
  services.
* *Boot.* `nd_main.c`, `nd_splash.c`. Small and self-contained; verified against golden 8.

**Wave 3 — integration (1 agent, sequential).** `nd_ui.c`, `nd_ui_input.c`,
`nd_applaunch.c`, `nd_core_loop.c`. This is where the construction order of §6, the tick
ordering in `read_keypress`, the `IncomingCall`→signal conversion and the process-per-app
lifecycle come together. It touches every other package and should not be parallelised.

**Not parallelisable and worth stating plainly:** the rasterizer must be one agent (two
agents each implementing "close enough" resampling produces frames that differ from each
other as well as from Python), and Wave 3 must be one agent (it is the file where every
ordering guarantee in this spec is enforced).

**Dependencies on other subsystems' specs:** `ModemService`, `BatteryService`,
`NotifyService`, `SettingsStorage`, `CrashHandler`/`ErrorScreen`, the UI framework
(`SoftKeyBar`, `AppSelector`, `MessageDialog`), the Dialer screens, the PhoneBook contact
selector, and `System.hw.pcf8575_keypad` / `i2c_keypad_setup`. The core needs only their
signatures, all of which are recorded in the Public Interface section above, so those
surveys and this one can proceed independently.

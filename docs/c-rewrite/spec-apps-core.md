# Stock apps (not Koki) — C port specification

**Subsystem:** the eleven shipped applications in
`neodct/overlay/NeoDCT/System/apps/` excluding `Koki/` and `Update/`.

**Python surveyed:** 3,048 lines across 15 `.py` files, 11 `manifest.json` files,
11 `icon.png` files, 1 `home.html`.

**Out of scope, covered elsewhere:** `apps/Koki/` (its own spec),
`apps/Update/` (`spec-update-system.md` §7), `System/engineering/apps/`
(developer tools), the widget internals (`spec-ui-framework.md`), the
launcher/app-scan/crash flow (`spec-core-loop.md` §9), the services the apps
talk to (`spec-core-services.md`).

**Cross-references used throughout.** Every widget these apps use —
`VerticalList`, `PagedList`, `TextInput`, `TextInputLong`, `MessageDialog`,
`TextScroller`, `InfoScreen`, `LevelSelector`, `SoftKeyBar`, `HeaderWidget` — is
specified pixel-by-pixel in `spec-ui-framework.md`. This document specifies only
what the *apps* do: which widget, with which arguments, in which order, and what
they draw themselves.

---

## What this does (plain English, for a reader who is not a C programmer)

These are the programs on the phone's menu. You scroll the launcher to one,
press the middle button, and it takes over the screen until you press Back
enough times to get out.

There are eleven of them and they are wildly different in size. **Clock** is
eighteen lines that put "This application has not been implemented yet." on
screen. **Music** is five hundred lines that read ID3 tags out of MP3 files,
pull album art out of them, and stream audio while drawing a progress bar. Most
sit in between.

Almost none of them draw anything themselves. They call ready-made screens —
"show me a list of these six words", "ask the user for a name", "show a warning
box" — from the shared UI framework, and then react to whichever item was
picked. That is why porting most of them is mechanical: once the framework
exists in C, an app like Settings is a menu, four branches, and a handful of
string constants.

Six of them keep data:

* **Phone book** and **Messages** and **Call log** use SQLite database files
  under `/NeoDCT/User/db/`. Same files, same tables, same columns as today —
  SQLite is a C library, so nothing changes there.
* **Settings**, **Tones**, **Games** and **Call log** save small preferences
  (chosen wallpaper, chosen ringtone, Snake level, top scores, call timers) in
  the plain-text `settings.prop` file.

Four of them reach outside the phone's own code:

* **Music** uses two Python libraries: `mutagen` to read the title/artist/album
  and cover art out of an MP3, and `miniaudio` to actually play sound.
  `miniaudio` is really a C library with a Python wrapper on top, so in C we
  drop the wrapper and call the C library directly — that one is nearly free.
  `mutagen` has no C equivalent, so we write a small ID3v2 reader ourselves;
  it only needs four tags out of the hundreds ID3 defines.
* **Tones** shells out to the `mpv` media player to preview a ringtone. That
  stays exactly as it is — it is already a separate program.
* **Browser** does nothing but launch NetSurf (a real web browser, already
  written in C, already shipping in the image) and then tidy up afterwards.
  Nothing about the browser itself is being ported.
* **Power** runs `poweroff` / `reboot`, and leaves a marker file when the user
  asks to reboot into recovery.

**Games** contains two: Snake and Memory. Both are grids of small white
rectangles on black with a handful of arithmetic — the sort of thing C is
actually good at. The one catch is that both shuffle/place things using Python's
random-number generator, and the project's reference screenshots captured the
exact food position that generator produced. Matching that pixel-for-pixel means
either copying Python's random-number generator into C, or re-taking that one
screenshot. That is written up under Risks.

**The one genuinely hard part is not any single app.** It is that Settings today
reaches directly into the launcher's own memory to change the wallpaper and to
add or remove the engineering apps from the menu. In the new design apps run in
their own separate process, so they cannot do that any more. The fix is easy —
Settings just saves the preference, and the launcher re-reads it when the app
exits — and it turns out to look identical on screen, because neither change is
visible until you are back at the menu anyway. But it has to be done
deliberately, not discovered late.

---

## Files and where they go in C

### Python source inventory

| Path (under `neodct/overlay/NeoDCT/System/apps/`) | LOC | Purpose |
| --- | ---: | --- |
| `MusicPlayer/main.py` | 516 | scan card, ID3 metadata + art, two playback backends, now-playing screen |
| `Games/main.py` | 104 | Games → Memory/Snake menus, top-score plumbing, instructions |
| `Games/memory.py` | 213 | Memory game: 8×5 board, 20 hand-drawn glyphs, scoring |
| `Games/snake.py` | 133 | Snake game: 29×14 grid, level→speed table, scoring |
| `Games/games_common.py` | 53 | key codes, direction table, `poll_key`, settings int helpers |
| `Messages/main.py` | 475 | Inbox/Outbox/Write, SMS send flow, contact-number field |
| `Settings/main.py` | 310 | Wallpaper / Memory card / Engineering Mode / About |
| `PhoneBook/main.py` | 214 | 7-entry menu, add/edit/erase, contact options submenu |
| `PhoneBook/shared/list_ui.py` | 72 | **shared** contact selector — used by PhoneBook, Messages **and the core** |
| `PhoneBook/__init__.py`, `PhoneBook/shared/__init__.py` | 0 | package markers (no C equivalent) |
| `Browser/main.py` | 261 | launch `netsurf-fb`, tag its stderr, drain input, repaint |
| `Tones/main.py` | 229 | Ringing Options / Ringing Tones with live mpv preview |
| `CallLog/main.py` | 188 | five call-log screens, timers, clear menus |
| `Calculator/main.py` | 159 | Nokia-style calculator with an Options operator menu |
| `Power/main.py` | 103 | Power off / Reboot / Recovery |
| `Clock/main.py` | 18 | "not implemented" placeholder |
| **total** | **3,048** | |

### Non-source files that ship with the apps

| Path | Size / shape | Notes |
| --- | --- | --- |
| `*/manifest.json` × 11 | see table below | read by the **core**, not by the app |
| `Browser/icon.png` | 120×120 RGBA, 607 B | |
| `Calculator/icon.png` | 120×120 RGBA, 2960 B | |
| `CallLog/icon.png` | 120×120 RGBA, 2933 B | |
| `Clock/icon.png` | 120×120 RGBA, 2656 B | |
| `Games/icon.png` | 120×120 **P** (palette), 2100 B | palette PNG — the loader must handle P→RGBA |
| `Messages/icon.png` | 120×120 **P**, 2670 B | |
| `MusicPlayer/icon.png` | **100×100** RGBA, 881 B | the odd one out |
| `PhoneBook/icon.png` | 120×120 **P**, 1708 B | |
| `Power/icon.png` | 120×120 RGBA, 561 B | |
| `Settings/icon.png` | 120×120 RGBA, 3957 B | |
| `Tones/icon.png` | 120×120 RGBA, 2370 B | |
| `Browser/home.html` | 2.6 KB | NetSurf start page, ships verbatim, **not ported** |

Three of the eleven icons are palette (`P`-mode) PNGs. `NeoDCT_UI.get_image()`
does `Image.open(path).convert("RGBA")`, so the PNG decoder in C must expand
palette images with a `tRNS` chunk to straight RGBA. See `spec-core-loop.md` for
the image cache; the apps never load their own icon.

### Manifest contents (exact)

| App dir | `name` | `id` | `icon` | `exec` |
| --- | --- | ---: | --- | --- |
| `PhoneBook` | `Phone book` | `1` | `icon.png` | `main.py` |
| `Messages` | `Messages` | `2` | `icon.png` | `main.py` |
| `CallLog` | `Call Log` | `3` | `icon.png` | `main.py` |
| `Settings` | `Settings` | `4` | `icon.png` | `main.py` |
| `Games` | `Games` | `6` | `icon.png` | `main.py` |
| `Calculator` | `Calculator` | `7` | `icon.png` | `main.py` |
| `Clock` | `Clock` | `8` | `icon.png` | `main.py` |
| `Tones` | `Tones` | `9` | `icon.png` | `main.py` |
| `Browser` | `Browser` | `11` | `icon.png` | `main.py` |
| `MusicPlayer` | `Music` | `970` | `icon.png` | `main.py` |
| `Power` | `Power` | `971` | `icon.png` | `main.py` |

(`Koki` = `Koki Mobile`/`10` and `Update` = `Update`/`12` complete the stock set
but belong to other specs. **There is no app with `id` 5.**) The core sorts the
launcher by integer `id`, so the launcher order is: Phone book 1, Messages 2,
Call Log 3, Settings 4, Games 6, Calculator 7, Clock 8, Tones 9, Koki Mobile 10,
Browser 11, Update 12, Music 970, Power 971 — then the engineering apps
(999, 9001–9006, 9990, 9997–9999) when engineering mode is on.

`id` is a **string in JSON** and `int()`-converted by the core
(`_scan_apps_from_dir`, defaulting to `999`). The manifest files differ in
whitespace (some tab-indented, some 4-space, some with a trailing newline and
some without) — irrelevant, they are parsed not compared.

### C destinations

Per `ARCHITECTURE.md`, every app becomes `apps/<Name>/app.so` sitting next to its
unchanged `manifest.json` and `icon.png`, `dlopen()`ed by `nd-apprun` in a
forked child. Shared code moves into `libneodct.so`.

| Python | C destination |
| --- | --- |
| `Clock/main.py` | `apps/Clock/app.c` |
| `Power/main.py` | `apps/Power/app.c` |
| `Calculator/main.py` | `apps/Calculator/app.c` |
| `CallLog/main.py` | `apps/CallLog/app.c` |
| `Tones/main.py` | `apps/Tones/app.c` |
| `Browser/main.py` | `apps/Browser/app.c` |
| `Browser/home.html` | unchanged, installed to the same absolute path |
| `PhoneBook/main.py` | `apps/PhoneBook/app.c` |
| **`PhoneBook/shared/list_ui.py`** | **`lib/nd_contacts.c` in `libneodct.so`** — the core and Messages both call it |
| `Settings/main.py` | `apps/Settings/app.c` |
| `Messages/main.py` | `apps/Messages/app.c` + `apps/Messages/msg_db.c` |
| `Games/main.py` | `apps/Games/app.c` |
| `Games/snake.py` | `apps/Games/snake.c` |
| `Games/memory.py` | `apps/Games/memory.c` |
| `Games/games_common.py` | `apps/Games/games_common.c/.h` (in-app, not libneodct) |
| `MusicPlayer/main.py` | `apps/MusicPlayer/app.c` + `music_meta.c` (ID3) + `music_audio.c` |
| `*/icon.png`, `*/manifest.json` | copied byte-for-byte into the image |

---

## Behaviour that must be reproduced exactly

### 0. Constants that every app in this subsystem assumes

```
ui.W            = 240
ui.H            = 175
ui.SOFTKEY_H    = 30
ui.content_bottom = 145
header_y        = max(30, int(H * 0.11)) = 30      (computed locally by 5 apps)
```

Six of the eleven apps recompute these with the `getattr(ui, "W", 240)` fallback
pattern rather than trusting the framework helper. In C they are struct fields;
keep the same three defaults (`ND_UI_W` 240, `ND_UI_H` 175, `ND_SOFTKEY_H` 30) so
a panel change has one edit point.

**Key codes used by the apps** (evdev, `EV_KEY` `value == 1` only):

| Code | Meaning | Code | Meaning |
| ---: | --- | ---: | --- |
| 14 | BACK / Clear | 103 | UP |
| 28 | ENTER / navi-centre | 108 | DOWN |
| 96 | KP-Enter (accepted as ENTER by Messages) | 105 / 106 | LEFT / RIGHT |
| 2…11 | digits `1 2 3 4 5 6 7 8 9 0` | 42 / 43 | `*` / `#` |
| 46 / 50 | legacy `c` / `m` (Clock only) | 52 | `.` on the dev keyboard |

**Colour literals used by the apps:** `"black"` = (0,0,0), `"white"` =
(255,255,255), `"gray"` = **(128,128,128)**, and three hex literals in
MusicPlayer: `"#cccccc"`, `"#999999"`, `"#333333"`.

**Fonts:** `font_s` = 14 px, `font_md` = 18 px, `font_n` = 20 px, `font_xl` =
24 px, all `/NeoDCT/System/ui/resources/fonts/font.ttf`.

### 0a. `VerticalList` / `PagedList` `app_id` is a *string*, not an int

`HeaderWidget.text_for()` formats with `"%s-%s"`. Apps pass ints (`1`, `4`, `6`,
`7`, `9`, `971`) **and** strings (`"1-1"`, `"1-3"`, `"1-4"`, `"1-6"`, `"1-1-3"`,
`"2-1"`, `"2-2"`, `"2-3"`, `"3-5"`). The C `nd_vlist`/`nd_pagedlist` `root_id`
field must therefore be `const char *`, and callers `snprintf` the composite ids.

### 0b. Widget-instance lifetime decides whether a menu remembers its position

This is observable and differs between apps. Reproduce exactly.

| App | Widget created… | Effect |
| --- | --- | --- |
| PhoneBook main menu | **once**, before the loop | selection & scroll window survive a trip into a submenu |
| CallLog root `PagedList` | **once**, before the loop | page survives |
| CallLog duration `PagedList` | **once**, before the loop | page survives |
| Messages root `PagedList` | **once**, before the loop | page survives |
| Settings root `VerticalList` | **inside** the loop | resets to "Wallpaper" every time |
| Tones root `PagedList` | **inside** the loop | resets to "Ringing Options" every time |
| Games menus (`_show_menu`) | **inside** the loop | reset to item 0 every time |
| Power menu | **inside** the loop | resets to "Power off" every time |
| MusicPlayer track list | **inside** the loop | resets to track 0 after every play |

---

### 1. Clock (`apps/Clock/main.py`, 18 lines)

Imports `SoftKeyBar` (**never used** — dead import) and `MessageDialog`.

```
run(ui):
    screen_w       = 240
    content_bottom = 145
    draw.rectangle((0, 0, 240, 145), fill="black")
    dialog = MessageDialog(ui, "This application has not been implemented yet.")
    fb.update(canvas)                       # presents the *blank* screen
    loop forever:
        dialog.show()                       # blocks; returns 28 or 14
        key = ui.wait_for_key()             # blocks AGAIN
        if key in (46, 28, 50): return
```

**Two quirks that must be reproduced:**

1. **It takes two key presses to leave.** `MessageDialog.show()` already consumes
   one key (28 or 14) to dismiss itself; then the app blocks on a *second*
   `wait_for_key()` and only 46, 28 or 50 exits. Press 14 twice and you loop and
   the dialog is redrawn.
2. The `MessageDialog` is constructed **before** the manual `fb.update()`, so the
   first thing presented is a plain black content area with whatever was in the
   softkey band; the dialog appears on the next flush inside `show()`.

The dialog itself: no title, icon
`/NeoDCT/System/ui/resources/img/errorscreen/warning.png` (24×24 RGBA), button
`"OK"`, `accept_keys=(28,)`, `cancel_keys=(14,)`, `margin=8`. The message wraps
to two lines at `font_n`, so `MessageDialog` takes its **centred** alert
branch. Golden frame: `app-clock.png`.

### 2. Power (`apps/Power/main.py`, 103 lines)

```c
#define POWER_APP_ID   971
#define STATE_DIR      "/NeoDCT/User/.ndsys"
#define RECOVERY_FLAG  "/NeoDCT/User/.ndsys/boot_recovery"
#define KEY_ENTER      28
static const char *MENU[3] = { "Power off", "Reboot", "Recovery" };
/* POWER_OFF=0, REBOOT=1, RECOVERY=2 */
static const char *const HALT_CMDS[3][2]   = {{"poweroff",0},{"/sbin/poweroff",0},{"busybox","poweroff"}};
static const char *const REBOOT_CMDS[3][2] = {{"reboot",0},  {"/sbin/reboot",0},  {"busybox","reboot"}};
```

`run(ui)` loop:

```
menu = VerticalList(ui, "Power", MENU, app_id=971)
SoftKeyBar(ui).update("Select", present=False)
choice = menu.show()
choice < 0  -> return
choice == 0 -> if _confirm("Switch the phone off?"):  _go_down(HALT_CMDS,   "Power off failed.")
choice == 1 -> if _confirm("Restart the phone?"):     _go_down(REBOOT_CMDS, "Reboot failed.")
choice == 2 -> if _confirm("Restart into recovery?"): _request_recovery()
```

* `_confirm(q)` = `MessageDialog(ui, q, title="Power", button_text="Yes").show() == 28`.
* `_tell(m)`   = `MessageDialog(ui, m, title="Power", button_text="OK").show()`.
* `_go_down(cmds, failure)`:
  1. `subprocess.call(["sync"])` — a **blocking** wait for the child.
  2. `_spawn_first(cmds)`: try each argv in order with `Popen`; the first that
     does not raise `OSError` (i.e. `execvp` found the binary) wins, return true.
  3. If none spawned → `_tell(failure)` and return to the menu.
  4. Otherwise `time.sleep(30)`. **Deliberate:** sit on the screen rather than
     bouncing back to the launcher while init tears the system down.
* `_request_recovery()`:
  1. `os.makedirs("/NeoDCT/User/.ndsys", exist_ok=True)`
  2. create/truncate `/NeoDCT/User/.ndsys/boot_recovery` (empty file)
  3. `subprocess.call(["sync"])`
  4. on any `OSError` → `_tell("Cannot ask for recovery: %s" % exc)` and **return
     without rebooting** (a read-only user partition must not reboot into a
     normal boot and look broken)
  5. else `_go_down(REBOOT_CMDS, "Reboot failed.")`

C notes: `Popen` here is fire-and-forget with **inherited** stdout/stderr.
`_spawn_first` distinguishes "binary not found" from "binary failed", which with
`fork()`+`execvp()` means the child must report exec failure back — use
`posix_spawnp` and check its return, or a close-on-exec pipe. Do **not** use
`system()`. Per `CODING-STANDARDS.md` §1.1, fork must be followed immediately by
exec.

`_APP_DIR` is prepended to `sys.path` at import time — a Python packaging
detail with no C equivalent.

### 3. Calculator (`apps/Calculator/main.py`, 159 lines)

```c
#define CALC_APP_ID  7
#define MAX_DIGITS   12
static const char *OPTIONS[5] = {"Equals","Add","Subtract","Multiply","Divide"};
/* OP_FOR_OPTION: 1->'+' 2->'-' 3->'*' 4->'/'  (0 = Equals) */
#define KEY_ENTER 28
#define KEY_BACK  14
#define KEY_STAR  42
#define KEY_HASH  43
/* DIGIT_KEYS: 2->'1' 3->'2' 4->'3' 5->'4' 6->'5' 7->'6' 8->'7' 9->'8' 10->'9' 11->'0' */
```

**State:** `entry` (string being typed, may be the literal `"Error"`), `acc`
(double or "none"), `pending_op` (one of `+ - * /` or none).

**`format_number(v)`** — reproduce exactly:

```
if isnan(v) or isinf(v):                       return "Error"
if v == trunc(v) and fabs(v) < 1e12:           text = "%lld" % (long long)v
else:                                          text = "%.8f" % v, then rstrip('0'), then rstrip('.')
if strlen(text) > MAX_DIGITS + 2  (i.e. > 14): text = "%.6e" % v
return text
```

`"%.6e"` must produce Python's form (`1.234560e+15`) — C's `%e` matches on glibc.

**`_fold()`** — folds `entry` into `acc` through `pending_op`:

```
if entry == "" or entry == "Error": return
value = strtod(entry)              (unparseable -> 0.0)
if acc is none or pending_op is none:  acc = value
else:
    '+' -> acc += value ; '-' -> acc -= value ; '*' -> acc *= value
    '/' -> if value == 0: { acc = none; pending_op = none; entry = "Error"; return }
           acc /= value
entry = ""
```

> **Critical C difference.** Python raises `ZeroDivisionError` on `x / 0.0`; C
> silently yields ±inf. The port **must** test the divisor explicitly and take
> the `"Error"` branch, otherwise state diverges (Python clears `acc` and
> `pending_op`; an inf would leave them set).

**`apply_option(choice)`:**

```
choice == 0 (Equals):
    _fold()
    if entry != "Error" and acc is not none: entry = format_number(acc)
    acc = none; pending_op = none
choice in 1..4:
    _fold()
    if entry != "Error": pending_op = OP_FOR_OPTION[choice]
```

**`type_digit(ch)`:**

```
if entry == "Error":                         entry = ""
if (entry == "0" or entry == "-0") and ch != '.':  drop the last char of entry
if strlen(entry with leading '-' stripped and all '.' removed) >= 12: return
entry += ch
```

**`type_point()`:**

```
if entry == "Error":     entry = ""
if entry contains '.':   return
entry += (entry == "" or entry == "-") ? "0." : "."
```

**`draw()`** — exact geometry:

```
draw.rectangle((0, 0, 240, 145), fill="black")
text = entry, or (acc is not none ? format_number(acc) : "0")
(w, h) = get_text_size(text, font_xl)
draw.text((max(5, 240 - 10 - w), 12), text, font=font_xl, fill="white")
if pending_op: draw.text((8, 16), pending_op, font=font_n, fill="white")
SoftKeyBar(ui).update("Options")            # present=True -> flushes
```

Note the display text is **right-aligned at x = 230 − w**, floored at x = 5.
The operator hint is drawn *after*, so it can overlap a 12-digit number.

**`open_options()`:**

```
menu = VerticalList(ui, "Options", OPTIONS, app_id=7)
SoftKeyBar(ui).update("OK", present=False)
choice = menu.show()
if choice >= 0: apply_option(choice)
```

**Main loop** (`loop()`), after one initial `draw()`:

```
key = wait_for_key()
28              -> open_options(); draw()
14              -> if entry non-empty:  entry = (entry=="Error" ? "" : entry[:-1]); draw()
                   elif acc or pending_op set: acc = none; pending_op = none; draw()
                   else: return                      # exits the app
2..11           -> type_digit(DIGIT_KEYS[key]); draw()
42, 43, 52      -> type_point(); draw()
anything else   -> ignored, no redraw
```

Golden frames: `app-calculator.png` (keys `2,3,4` → "123" right-aligned) and
`app-calculator-options.png` (keys `8,28` → the Options list over "7").

### 4. CallLog (`apps/CallLog/main.py`, 188 lines)

```c
#define CALLLOG_APP_ID 3
#define CALLLOG_DB     "/NeoDCT/User/db/call_log.db"
static const char *ROOT_ITEMS[5] = {
    "Missed calls", "Received calls", "Dialed calls",
    "Clear call lists", "Show call duration" };
static const char *CLEAR_ITEMS[4] = {"All", "Missed", "Dialed", "Received"};
static const char *DURATION_ITEMS[4] = {
    "Last call duration", "Received calls' duration",
    "Dialed calls' duration", "Clear timers" };
/* settings keys */
"calllog.duration.last"  "calllog.duration.received"  "calllog.duration.dialed"
```

**Schema** — created by the app on every connect (the core does *not* create
this one, and **no `PRAGMA journal_mode=WAL`** is issued here, unlike the three
core databases):

```sql
CREATE TABLE IF NOT EXISTS calls
  (id INTEGER PRIMARY KEY AUTOINCREMENT,
   type TEXT,             -- 'missed' | 'received' | 'dialed'
   number TEXT,
   timestamp INTEGER,
   duration INTEGER DEFAULT 0)
```

`_connect()` first `os.makedirs("/NeoDCT/User/db", exist_ok=True)`.

**Queries:**

```sql
SELECT number, timestamp FROM calls WHERE type=? ORDER BY id DESC LIMIT 20
DELETE FROM calls                     -- clear all
DELETE FROM calls WHERE type=?        -- clear one kind
```

Every DB call is wrapped: on exception print
`[CallLog] DB read failed: {exc}` / `[CallLog] DB clear failed: {exc}` and return
`[]` / `False`.

**Formatting:**

```
format_duration(s):  s = max(0, (int)s);  "%02d:%02d:%02d" % (s/3600, (s%3600)/60, s%60)
format_call_time(t): strftime("%d.%m. %H:%M", localtime(t)); on any failure -> ""
```

**Screens:**

* `run(ui)`: `PagedList(ui, "Call log", ROOT_ITEMS, root_id=3)` created **once**
  outside the loop (page persists). `show()` → `-1` exits; 0/1/2 →
  `show_call_list`; 3 → `show_clear_menu`; 4 → `show_duration_menu`.
* `show_call_list(ui, title, type)`:
  * `fetch_calls(type)` once. Empty → `InfoScreen(ui, title, "No numbers").show()`
    then return.
  * Loop: items = `number or "Unknown"`;
    `VerticalList(ui, title, items, app_id=3)` (rebuilt every iteration, so the
    selection resets after viewing a detail); `SoftKeyBar(ui).update("Details",
    present=False)`; `show()`; `-1` returns; otherwise
    `InfoScreen(ui, number or "Unknown", format_call_time(ts)).show()`.
  * Titles: `"Missed calls"`, `"Received calls"`, `"Dialed calls"`; types
    `"missed"`, `"received"`, `"dialed"`.
* `show_clear_menu(ui)`: `VerticalList(ui, "Clear call lists", CLEAR_ITEMS,
  app_id=3)`, `SoftKeyBar` `"OK"` `present=False`; index → type map
  `{0: NULL, 1: "missed", 2: "dialed", 3: "received"}` (note 2↔3 are *not* in
  list order relative to `ROOT_ITEMS`); on success
  `InfoScreen(ui, "List cleared", softkey_text="OK").show()`.
* `show_duration_menu(ui)`: `PagedList(ui, "Call duration", DURATION_ITEMS,
  root_id="3-5")` created **once**. 0/1/2 → `InfoScreen(ui, <the same label as
  the menu entry>, format_duration(get_setting_int(key))).show()`; 3 → set all
  three timer settings to `"0"` then `InfoScreen(ui, "Timers cleared",
  softkey_text="OK").show()`.
* Timer read: `int(get_setting(key, "0") or 0)`, any exception → 0. Write:
  `set_setting(key, str(int(seconds)))`, exception →
  `[CallLog] Timer write failed: {exc}` and `False`.

Golden frame: `app-calllog.png`.

### 5. Games (`apps/Games/`, 503 lines total)

#### 5a. `games_common.py`

```c
#define GAMES_APP_ID 6
#define KEY_ENTER 28
#define KEY_BACK  14
#define KEY_UP   103
#define KEY_DOWN 108
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_NUM_2  3
#define KEY_NUM_4  5
#define KEY_NUM_5  6
#define KEY_NUM_6  7
#define KEY_NUM_8  9
/* DIR_KEYS -> (dx, dy) */
103:(0,-1)  3:(0,-1)   108:(0,1)  9:(0,1)
105:(-1,0) 5:(-1,0)    106:(1,0)  7:(1,0)
```

`poll_key(ui, timeout)` = `ui.read_keypress(timeout)`; if that raises, or the
attribute is missing, `time.sleep(timeout)` and return none.

`get_setting_int(key, default)` = `int(get_setting(key, str(default)) or default)`,
any exception → `default`.
`set_setting_value(key, value)` = `set_setting(key, str(value))`, on exception
print `[Games] Setting write failed ({key}): {exc}` and return `False`.

#### 5b. `Games/main.py` — the menus

```c
"games.snake.level"      /* default 5 */
"games.snake.topscore"   /* default 0 */
"games.memory.topscore"  /* default 0 */
static const char *SNAKE_MENU[4]  = {"New game","Level","Top score","Instructions"};
static const char *MEMORY_MENU[3] = {"New game","Top score","Instructions"};
```

`_show_menu(ui, title, items)` = `VerticalList(ui, title, items, app_id=6)` +
`SoftKeyBar(ui).update("Select", present=False)` + `show()`.

`run(ui)` loop: `_show_menu(ui, "Games", ["Memory", "Snake"])`; `<0` exits;
0 → `memory_menu`; 1 → `snake_menu`. **Memory is first.**

`snake_menu(ui)` loop over `_show_menu(ui, "Snake", SNAKE_MENU)`:

```
0 New game     -> level = get_setting_int("games.snake.level", 5)
                  score = SnakeGame(ui, level).play()
                  _finish_game(ui, score, "games.snake.topscore")
1 Level        -> current = get_setting_int("games.snake.level", 5)
                  picked = LevelSelector(ui, current=current, app_id=6).show()
                  if picked is not None: set_setting_value("games.snake.level", picked)
2 Top score    -> InfoScreen(ui, "Top score", get_setting_int("games.snake.topscore", 0)).show()
3 Instructions -> TextScroller(ui, SNAKE_INSTRUCTIONS).show()
```

`memory_menu(ui)` is the same shape with `MEMORY_MENU`, no Level entry, and
`"games.memory.topscore"`.

`_finish_game(ui, score, top_key)`:

```
if score is None: return                       # the player pressed Back
top = get_setting_int(top_key, 0)
if score > top:
    set_setting_value(top_key, score)
    InfoScreen(ui, "New top score:", score, softkey_text="OK").show()
else:
    InfoScreen(ui, "Game over! Score:", score, softkey_text="OK").show()
```

**Instruction strings, verbatim (single lines, no embedded newlines):**

```
SNAKE_INSTRUCTIONS =
"Feed the snake by steering it to the food. Every bite makes it grow longer. Use keys 2, 4, 6 and 8 to change direction. The game ends if the snake runs into the walls or into its own body. A higher level means more speed and more points for each bite."

MEMORY_INSTRUCTIONS =
"All the cards lie face down. Move the cursor with keys 2, 4, 6 and 8 and turn a card over with key 5. Two matching cards are cleared from the board. Find every pair to finish the game. The fewer tries you need, the higher your score."
```

`LevelSelector(ui, current, count=9, title="Level", app_id=6)` builds
`["Level 1" … "Level 9"]`, presets `selected_index = clamp(current-1, 0, 8)`,
draws `SoftKeyBar` `"OK"` `present=False`, and returns `choice + 1` or none.

Golden frame: `app-games.png` (the "Games" list).

#### 5c. Snake (`snake.py`)

```c
#define GRID_W 29
#define GRID_H 14
#define CELL    8
/* derived on 240x175: */
score_h  = 20
board_x  = (240 - 29*8) / 2 = 4
board_y  = 20 + 4           = 24
board_w  = 232
board_h  = 112
```

`tick_delay(level) = fmax(0.09, 0.40 - 0.033 * level)`. Exact values:

| level | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| delay (s) | 0.367 | 0.334 | 0.301 | 0.268 | 0.235 | 0.202 | 0.169 | 0.136 | 0.103 |

The 0.09 floor is never reached for levels 1–9. `level` is clamped to 1…9 in the
constructor.

**Initial state:** `cx = 29/2 = 14`, `cy = 14/2 = 7`;
`snake = [(15,7), (14,7), (13,7)]` (head first); `direction = (1,0)`;
`turn_queue = []`; `score = 0`; `random.seed(time.time())`; then `spawn_food()`.

**`spawn_food()`:** build `open_cells` as
`[(x,y) for x in 0..28 for y in 0..13 if (x,y) not in set(snake)]` — **x-major
order** — then `food = random.choice(open_cells)` (or none if the board is full).

**`queue_turn(new_dir)`:**

```
last = turn_queue.back() if turn_queue else direction
if (new_dir.x + last.x, new_dir.y + last.y) == (0,0): return   # no 180s
if new_dir != last and len(turn_queue) < 2: turn_queue.push_back(new_dir)
```

**`step()` → bool (false = death):**

```
if turn_queue: direction = turn_queue.pop_front()
(nx, ny) = head + direction
if nx < 0 or nx >= 29 or ny < 0 or ny >= 14: return false      # wall
tail = snake.back()
body = set(snake) minus ({tail} if new_head != food else {})   # the tail frees up unless we grow
if new_head in body: return false                              # bit itself
snake.push_front(new_head)
if food and new_head == food: score += level; spawn_food()
else: snake.pop_back()
return true
```

**`_cell_rect(x, y)` = `(4 + 8x + 1, 24 + 8y + 1, 4 + 8x + 6, 24 + 8y + 6)`** —
a 6×6 inclusive rectangle inside each 8-px cell.

**`render()`:**

```
draw.rectangle((0, 0, 240, 175), fill="black")     # the WHOLE screen, softkey band included
draw.text((4, 1), str(score), font=font_md, fill="white")
draw.rectangle((2, 22, 237, 137), outline="white") # board_x-2, board_y-2, +board_w+1, +board_h+1
if food: draw.rectangle(_cell_rect(food), outline="white")
for each (x,y) in snake: draw.rectangle(_cell_rect(x,y), fill="white")
fb.update(canvas)
```

**No softkey is ever drawn during play** — the bottom 30 rows stay black.
(Verified against `game-snake.png`: no ink below y = 137.)

**`play()`:**

```
render()
next_move = now() + tick_delay(level)
loop:
    timeout = max(0.0, next_move - now())
    key = poll_key(ui, min(timeout, 0.05))         # never waits more than 50 ms
    if key == 14: return NONE                       # quit, no score
    if key in DIR_KEYS: queue_turn(DIR_KEYS[key])
    if now() >= next_move:
        if not step(): render(); sleep(0.4); return score
        render()
        next_move = now() + tick_delay(level)
```

Golden frame `game-snake.png`: score `0` at (4,1); body cells (13,7),(14,7),(15,7)
filled; food at grid cell **(5,12)** drawn as an outline. See Risks §R1 — that
food position is what Python's `random.choice` produced and is the only
randomness-dependent pixel in the golden set.

#### 5d. Memory (`memory.py`)

```c
#define COLS 8
#define ROWS 5
#define PAIR_BASE     10   /* points for a pair found first try  */
#define MISS_PENALTY   2   /* deducted per miss since last match */
#define PAIR_MIN       2   /* floor                              */
#define REVEAL_SECS  0.9
```

**Derived on 240×175:**

```
usable_h = content_bottom - 8 = 137
cell     = min((240 - 8) / 8, 137 / 5) = min(29, 27) = 27
board_x  = (240 - 8*27) / 2 = 12
board_y  = (145 - 5*27) / 2 =  5
_card_rect(col,row) = (12 + 27c + 2, 5 + 27r + 2, 12 + 27c + 24, 5 + 27r + 24)
                       /* i.e. px+2 … px+cell-3 : a 23x23 inclusive box */
```

**Setup:** `kinds = list(range(20)) * 2` (40 entries: 0,1,…,19,0,1,…,19);
`random.seed(time.time())`; `random.shuffle(kinds)`; `cards = kinds`;
`state = ["down"] * 40`; `cursor = (0,0)`; `first_pick = none`; `score = 0`;
`misses = 0`. Index = `row * 8 + col`.

**`render()`:**

```
draw.rectangle((0, 0, 240, 175), fill="black")     # whole screen; no softkey ever
for row 0..4, col 0..7:
    rect = _card_rect(col, row)
    "down" -> draw.rectangle(rect, fill="white")
    "up"   -> draw.rectangle(rect, outline="white")
              glyph_box = (rect.x0+4, rect.y0+4, rect.x1-4, rect.y1-4)
              draw_glyph(draw, glyph_box, cards[idx], "white")
    "gone" -> nothing
# cursor ring, drawn in the gap around the current cell:
px = 12 + 27*col ; py = 5 + 27*row
draw.rectangle((px, py, px + 26, py + 26), outline="white")
if state[cursor] == "down":
    draw.rectangle(_card_rect(col, row), outline="black")   # black inner border on the white card
fb.update(canvas)
```

**`draw_glyph(draw, box, kind, color)`** — 20 shapes. With
`box = (x0,y0,x1,y1)`, `cx = (x0+x1)/2`, `cy = (y0+y1)/2`, `w = x1-x0`
(on this hardware `w = 14`, `box = (px+6, py+6, px+20, py+20)`):

| kind | drawing |
| ---: | --- |
| 0 | `ellipse(box, fill)` |
| 1 | `ellipse(box, outline)` and `ellipse((x0+3,y0+3,x1-3,y1-3), outline)` |
| 2 | `rectangle(box, fill)` |
| 3 | `rectangle(box, outline)` |
| 4 | `polygon([(cx,y0),(x1,cy),(cx,y1),(x0,cy)], fill)` |
| 5 | same polygon, `outline` |
| 6 | `polygon([(cx,y0),(x1,y1),(x0,y1)], fill)` — triangle up |
| 7 | `polygon([(x0,y0),(x1,y0),(cx,y1)], fill)` — triangle down |
| 8 | `t = max(2, w/4)`; `rectangle((cx-t/2, y0, cx+t/2, y1), fill)`; `rectangle((x0, cy-t/2, x1, cy+t/2), fill)` |
| 9 | `line((x0,y0,x1,y1), fill, width=2)`; `line((x0,y1,x1,y0), fill, width=2)` |
| 10 | `q = max(1, w/6)`; `polygon([(cx,y0),(cx+q,cy-q),(x1,cy),(cx+q,cy+q),(cx,y1),(cx-q,cy+q),(x0,cy),(cx-q,cy-q)], fill)` |
| 11 | `r = max(2, w/4)`; `ellipse((x0,y0,x0+2r,y0+2r), fill)`; `ellipse((x1-2r,y0,x1,y0+2r), fill)`; `polygon([(x0,y0+r),(x1,y0+r),(cx,y1)], fill)` |
| 12 | `for yy in range(y0, y1+1, 4): line((x0,yy,x1,yy), fill, width=2)` |
| 13 | `for xx in range(x0, x1+1, 4): line((xx,y0,xx,y1), fill, width=2)` |
| 14 | `step = max(3, w/3)`; for `i, yy in enumerate(range(y0,y1,step))`, `j, xx in enumerate(range(x0,x1,step))`: if `(i+j)%2==0` `rectangle((xx, yy, min(xx+step-1,x1), min(yy+step-1,y1)), fill)` |
| 15 | `polygon([(x0,y0),(x1,y0),(x0,y1),(x1,y1)], fill)` — self-crossing "hourglass"; **reproduce Pillow's exact scanline fill for a self-intersecting polygon** |
| 16 | `t = max(2, w/4)`; `polygon([(cx,y0),(x1,cy),(x0,cy)], fill)`; `rectangle((cx-t/2, cy, cx+t/2, y1), fill)` |
| 17 | `t = max(2, w/4)`; `polygon([(x1,cy),(cx,y0),(cx,y1)], fill)`; `rectangle((x0, cy-t/2, cx, cy+t/2), fill)` |
| 18 | `r = max(2, w/3)`; four `rectangle`s of side `r` at each corner |
| 19 (else) | `rectangle(box, outline)`; `r = max(1, w/5)`; `ellipse((cx-r, cy-r, cx+r, cy+r), fill)` |

All divisions are Python integer floor division (`//`) on non-negative values, so
plain C `/` on `int32_t` matches. With `w = 14`: `t = 3`, `q = 2`, `r` = 3 (kind
11), 4 (kind 18), 2 (kind 19), `step = 4` (kind 14).

**`move_cursor(dx, dy)`:** `cursor = ((col+dx) mod 8, (row+dy) mod 5)` — **wraps**.
Python's `%` on negatives yields a non-negative result; C's does not. Use
`((v % n) + n) % n`.

**`flip()`:**

```
idx = row*8 + col
if state[idx] != "down": return
state[idx] = "up"
if first_pick is none: first_pick = idx; render(); return
first = first_pick; first_pick = none; render()
if cards[first] == cards[idx]:
    score += max(2, 10 - 2*misses)
    misses = 0
    sleep(0.25); state[first] = "gone"; state[idx] = "gone"
else:
    misses += 1
    sleep(0.9);  state[first] = "down"; state[idx] = "down"
render()
```

**`play()`:**

```
render()
loop:
    key = poll_key(ui, 0.1)
    if key is none: continue
    if key == 14: return NONE
    if key in DIR_KEYS: move_cursor(...); render()
    elif key in (28, 6):            # ENTER or the digit '5'
        flip()
        if all cards "gone": sleep(0.3); return score
```

Golden frame `game-memory.png`: all 40 cards face-down, cursor ring at
(12,5)–(38,31) with the black inner border on card (0,0). The shuffle is not
visible in that frame.

### 6. PhoneBook (`apps/PhoneBook/`, 286 lines)

#### 6a. Import-time side effect

`_ensure_serial_redirect()` runs at module import: it reads
`System.core.main.SERIAL_CONSOLE_DEVICE` (`os.environ.get("NEODCT_SERIAL_DEVICE",
"/dev/ttyAMA0")`) and, unless `sys.stdout.name` already equals it, reopens that
device for writing and rebinds `sys.stdout` and `sys.stderr` to it. All errors
swallowed. In C: on app start, if `NEODCT_SERIAL_DEVICE`/`/dev/ttyAMA0` opens,
`dup2` it onto fds 1 and 2. Ignore any failure.

#### 6b. `shared/list_ui.py` — the shared contact selector

**Lives in `libneodct.so`, not in the PhoneBook app.** Three callers: PhoneBook,
Messages (`ContactNumberInput`), and `core/main.py:handle_input` (home-screen
UP/DOWN opens it with `title="Select", btn_text="Call"`).

```c
#define PHONEBOOK_DB "/NeoDCT/User/db/phonebook.db"
```

`get_all_contacts(search_query=None)`:

```sql
-- with a query (LIKE with wildcards on both sides, SQLite LIKE is
-- case-insensitive for ASCII by default):
SELECT * FROM contacts WHERE name LIKE ? ORDER BY name ASC        -- '%'||q||'%'
-- without:
SELECT * FROM contacts ORDER BY name ASC
```

Rows are `(id, name, number, speed_dial)`. **No exception handling** — a missing
database file makes `sqlite3.connect` create an empty one and the `SELECT` then
raises. In C, return an empty set rather than crashing, and note the deviation.

`show_contact_selector(ui, title="Contacts", btn_text="Select",
search_query=None, header_root="1")`:

```
contacts = get_all_contacts(search_query)
if empty:
    draw.rectangle((0, 0, 240, 145), fill="black")
    msg = search_query ? "No Results" : "No Contacts"
    (w,h) = get_text_size(msg, font_n)
    draw.text(((240-w)/2, max(10, (145-h)/2)), msg, font=font_n, fill="white")
    fb.update(canvas); sleep(1.5); return NONE
names = [row.name for row in contacts]
vlist = VerticalList(ui, title, names, app_id=header_root)
loop (executes exactly once):
    SoftKeyBar(ui).update(btn_text)             # present=True -> flush
    i = vlist.show()
    if i == -1: return NONE
    return (contacts[i], i)
```

Golden frame: `contacts-picker.png`.

#### 6c. `PhoneBook/main.py`

```c
static const char *MAIN_ITEMS[7] = {
    "Search",           /* 0 */
    "Add entry",        /* 1 */
    "Edit",             /* 2 */
    "Erase",            /* 3 */
    "Send entry",       /* 4  -- DOES NOTHING */
    "Options",          /* 5 */
    "1-touch dialing"   /* 6  -- DOES NOTHING */
};
```

`run(ui)` builds `VerticalList(ui, "Phonebook", MAIN_ITEMS, app_id=1)` **once**,
then loops: `softkey.update("Select")` (present=True), `sel = main_list.show()`.

```
-1 -> return
 0 Search:  query = TextInput(ui, "Search", "Name:", input_filter="letters").show()
            if query (non-empty and not None):
                r = show_contact_selector(ui, title="Results", btn_text="Options",
                                          search_query=query, header_root="1-1")
                if r: run_contact_options(ui, r.contact, "1-1-%d" % (r.index + 1))
 1 Add entry -> add_entry_action(ui)
 2 Edit:    r = show_contact_selector(ui, title="Edit", btn_text="Edit", header_root="1-3")
            if r: edit_contact_action(ui, r.contact)
 3 Erase:   r = show_contact_selector(ui, title="Erase", btn_text="Erase", header_root="1-4")
            if r: delete_contact_action(ui, r.contact)
 4 -> nothing (falls through the if/elif chain)
 5 -> run_options_submenu(ui)
 6 -> nothing
```

Note `if query:` — an **empty** search string is treated as cancel, whereas
`edit_contact_action` uses `is None` and therefore accepts an empty name/number.

`add_entry_action(ui)`:

```
name   = TextInput(ui, "Add Entry", "Name:",   input_filter="letters").show(); if falsy: return
number = TextInput(ui, "Add Entry", "Number:", input_filter="numbers").show(); if falsy: return
INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, 0)
_draw_center_message(ui, "Saved!")
on exception: print "[PB] Save Error: {e}"  (no dialog)
```

`edit_contact_action(ui, contact)`:

```
new_name   = TextInput(ui, "Edit Name",   "Name:",   initial_text=contact.name,
                       input_filter="letters").show();  if new_name is None: return
new_number = TextInput(ui, "Edit Number", "Number:", initial_text=contact.number,
                       input_filter="numbers").show();  if new_number is None: return
UPDATE contacts SET name=?, number=? WHERE id=?
_draw_center_message(ui, "Updated!")
on exception: print "[PB] Update Error: {e}"
```

`delete_contact_action(ui, contact)`: `DELETE FROM contacts WHERE id=?`, then
`_draw_center_message(ui, "Erased")` — **no exclamation mark**, and **no
confirmation dialog** (the Python comment says one is planned for M3). No
exception handling.

`_draw_center_message(ui, text, duration=1.0, font=font_xl, fill="white")`:

```
draw.rectangle((0, 0, 240, 145), fill="black")
(w,h) = get_text_size(text, font)
draw.text(((240-w)/2, max(10, (145-h)/2)), text, font=font, fill=fill)
fb.update(canvas)
sleep(duration)
```

`run_contact_options(ui, contact, header_root)`:

```
items = ["Call", "Edit", "Delete", "Send number"]
options_list = VerticalList(ui, contact.name, items, app_id=header_root)   # built once
loop:
  softkey.update("Select")
  sel = options_list.show()
  -1 -> return
   0 Call:  draw.rectangle((0, 0, 240, 145), fill="black")
            y = max(12, int(145 * 0.30)) = 43
            draw.text((10, 43),  "Calling...",     font=font_xl, fill="white")
            draw.text((10, 78),  contact.name,     font=font_n,  fill="white")
            draw.text((10, 103), contact.number,   font=font_s,  fill="white")
            fb.update(canvas); sleep(2)
            /* NO ACTUAL DIAL -- the modem is never touched. Reproduce as-is. */
   1 Edit:   edit_contact_action(ui, contact); return
   2 Delete: delete_contact_action(ui, contact); return
   3 Send number: _draw_center_message(ui, "Sent!")   /* stub */
```

`run_options_submenu(ui)`: `VerticalList(ui, "Options", ["Type of view",
"Memory status"], app_id="1-6")`, `softkey.update("Select")`; item 0 prints
`Changing View Type...`, item 1 prints `Checking Memory...` — **stdout only, no
screen output**.

**Schema** (created by the core, not the app — `spec-core-loop.md`
`init_databases`), reproduced here because PhoneBook depends on the exact
columns:

```sql
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS contacts
  (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, number TEXT, speed_dial INTEGER);
-- seeded when empty: ("NeoDCT Support", "555-1234", 2)
```

Golden frame: `app-phonebook.png`.

### 7. Messages (`apps/Messages/main.py`, 475 lines)

```c
#define ROOT_ID_MESSAGES 2
#define SMS_MAX_CHARS    160        /* one GSM text-mode segment */
#define DB_DIR           "/NeoDCT/User/db"
#define INBOX_DB         "/NeoDCT/User/db/sms_inbox.db"
#define OUTBOX_DB        "/NeoDCT/User/db/sms_outbox.db"
static const int ARROW_KEYS[4] = {103, 105, 106, 108};
```

**Schemas** (inbox and outbox created by the core with
`PRAGMA journal_mode=WAL`; the outbox is *also* created lazily by this app in
`_save_outbox_message`, **without** the WAL pragma):

```sql
CREATE TABLE IF NOT EXISTS inbox
  (id INTEGER PRIMARY KEY AUTOINCREMENT, message TEXT, sender TEXT,
   timestamp INTEGER, is_read INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS outbox
  (id INTEGER PRIMARY KEY AUTOINCREMENT, message TEXT, timestamp INTEGER);
```

**All queries** (each opens and closes its own connection; every read first
checks `os.path.exists(db)` and returns `[]`/`None` when absent):

```sql
SELECT id, message, sender, timestamp, is_read FROM inbox ORDER BY timestamp DESC
SELECT id, message, timestamp FROM outbox ORDER BY timestamp DESC
SELECT id, message, sender, timestamp, is_read FROM inbox WHERE id = ?
UPDATE inbox SET is_read = 1 WHERE id = ?
DELETE FROM inbox  WHERE id = ?
DELETE FROM outbox WHERE id = ?
INSERT INTO outbox (message, timestamp) VALUES (?, ?)      -- ts = (int)time()
```

`_save_outbox_message` first `os.makedirs("/NeoDCT/User/db", exist_ok=True)`.

**`_screen_metrics(ui)`** returns `(240, 175, 145, header_y=max(30, int(175*0.11))=30)`.

**`_format_timestamp(ts)`:** falsy `ts` → `"Unknown time"`; else
`strftime("%Y-%m-%d %H:%M", localtime(ts))`.

**`_wrap_text(ui, text, max_width, font)`** — the app's *own* wrapper, distinct
from the framework ones:

```
words = text.split()          # ANY whitespace, so newlines are lost
if no words: return [""]
for each word:
    candidate = current ? current + " " + word : word
    if width(candidate) <= max_width: current = candidate; continue
    if current: lines.push(current); current = word
    else:  # single word wider than the line
        trimmed = word; while trimmed and width(trimmed + "...") > max_width: drop last char
        lines.push(trimmed ? trimmed + "..." : "..."); current = ""
if current: lines.push(current)
```

#### Screens

**`run(ui)`** — `PagedList(ui, "Messages", ["Inbox", "Outbox", "Write Message"],
root_id=2, show_select_hint=True)` built **once**; 0 → `_show_inbox(ui, 2, 1)`,
1 → `_show_outbox(ui, 2, 2)`, 2 → `_show_write_message(ui, 2, 3)`, `<0` → exit.
Golden frame: `app-messages.png`.

**`_show_inbox(ui, root_id=2, sub_index=1)`** loop:

```
messages = fetch inbox (newest first)
if empty: _show_empty_state(ui, "Inbox", "2-1", None, "No Messages"); return
items = [ is_read ? sender : ("* " + sender) for each row ]
header_root = "2-1"
v_list = VerticalList(ui, "Inbox", items, app_id="2-1")     # rebuilt each iteration
SoftKeyBar(ui).update("Open", present=False)
i = v_list.show(); if i == -1: return
_mark_read(id)
r = _show_message_detail(ui, "Inbox", "2-1", i + 1, message, message_id=id,
                         sender=sender, timestamp=ts)
if r == "deleted": continue        # else fall out of the loop and return
```

Note the loop only continues on `"deleted"`; a normal Back out of the detail
screen ends `_show_inbox` entirely. Golden frame: `app-messages-inbox.png`
("No Messages").

**`_show_outbox(ui, 2, 2)`** — same shape, `header_root = "2-2"`, items are the
**message bodies** themselves, no `_mark_read`, detail title `"Outbox"`.

**`_show_empty_state(ui, title, root_id, sub_index, message)`:**

```
draw.rectangle((0, 0, 240, 175), fill="black")        # FULL screen
draw.text((5, 5), title, font=font_xl, fill="white")
draw.line((0, 30, 240, 30), fill="white")
HeaderWidget(ui, root_id).draw(sub_index)             # root_id is "2-1"; sub_index None -> "2-1"
(w, h) = get_text_size(message, font_n)
y = 30 + max(0, ((145 - 30) - h) / 2)
draw.text(((240 - w)/2, y), message, font=font_n, fill="white")
SoftKeyBar(ui).update("Back", present=False)
fb.update(canvas)
wait until key == 14, then return                     # ONLY 14 exits
```

**`_show_message_detail(ui, title, root_id, sub_index, message, message_id, sender, timestamp)`:**

Precomputed once:
`timestamp_text = _format_timestamp(timestamp)`;
`meta_lines = (["From: " + sender] if sender else []) + ["Time: " + timestamp_text]`;
`body_lines = _wrap_text(ui, message, max(20, 240-20)=220, font_n)`.

Redraw loop:

```
draw.rectangle((0, 0, 240, 175), fill="black")        # FULL screen
draw.text((5, 5), title, font=font_xl, fill="white")
draw.line((0, 30, 240, 30), fill="white")
HeaderWidget(ui, root_id).draw(sub_index)
y = 40
for line in meta_lines:  if y > 127: break; draw.text((10, y), line, font=font_s, fill="gray"); y += 18
y += 4
for line in body_lines:  if y > 123: break; draw.text((10, y), line, font=font_n, fill="white"); y += 22
SoftKeyBar(ui).update("Options", present=False)
fb.update(canvas)
key = wait_for_key()
14      -> return (nothing)
28 / 96 -> options, below
```

The two cut-offs are `content_bottom - 18 = 127` (meta) and
`content_bottom - 22 = 123` (body).

Options branch, chosen by comparing the **title string**:

```
title == "Inbox":
    VerticalList(ui, "Options", ["Just Erase for now"], app_id=root_id)
    sel == 0 and message_id is not none ->
        DELETE FROM inbox; MessageDialog(ui, "Erased!").show(); return "deleted"
title == "Outbox":
    VerticalList(ui, "Options", ["Erase", "Send"], app_id=root_id)
    sel == 0 and message_id is not none ->
        DELETE FROM outbox; MessageDialog(ui, "Erased!").show(); return "deleted"
    sel == 1 -> _send_message_flow(ui, message, root_id, sub_index)
```

No `SoftKeyBar` is set before those `VerticalList.show()` calls — whatever the
previous screen left in the softkey band persists. Reproduce.

**`ContactNumberInput(TextInput)`** — Send-To field, `input_filter="numbers"`.
Its `show()` overrides the base:

```
softkey = SoftKeyBar(ui); softkey.update("OK")
cursor_on = true; last_blink = now(); draw(cursor_on)
loop:
    if now() - last_blink > 0.5: cursor_on = !cursor_on; last_blink = now(); draw(cursor_on)
    key = wait_for_key(); if none: continue
    if key in (103, 105, 106, 108):                       # ANY arrow opens the picker
        picked = show_contact_selector(ui, title="Contacts", btn_text="OK",
                                       header_root="2-3")
        if picked: text = str(picked.contact.number)
        draw(cursor_on); softkey.update("OK"); continue
    action = handle_key(key)
    "confirm" -> return text ; "cancel" -> return NONE
    "typed" | "backspace" | "mode" -> draw(cursor_on)
```

`input_filter="numbers"` means the T9 engine has only `MODE_123`, so on the i2c
keypad digits, `*` and `#` type **literally** — no multi-tap letters. Covered by
`test_messages_number_field.py`.

**`_draw_sending(ui, number)`:**

```
draw.rectangle((0, 0, 240, 145), fill="black")
(w, h) = get_text_size("Sending...", font_n)
y = max(10, (145 - h)/2 - 12)
draw.text(((240-w)/2, y), "Sending...", font=font_n, fill="white")
(w2, _) = get_text_size(number, font_s)
draw.text(((240-w2)/2, y + h + 8), number, font=font_s, fill="gray")
SoftKeyBar(ui).update("", present=False)      # empty string -> bar cleared, no text
fb.update(canvas)
```

**`_send_message_flow(ui, text, root_id, sub_index) -> bool`:**

```
text = strip(text)
if empty:          MessageDialog(ui, "Message is empty!").show(); return false
if len > 160:      MessageDialog(ui, "Too long for one SMS (%d/%d)." % (len, 160)).show(); return false
number = ContactNumberInput(ui, "Send To", "Number:").show()
if number is none: return false
number = keep only characters in "0123456789*#+"
if number empty:   MessageDialog(ui, "No number given!").show(); return false
modem = getattr(ui, "modem", None)
if modem is none:  MessageDialog(ui, "ModemService is not running.").show(); return false
_draw_sending(ui, number)
(ok, detail) = modem.send_sms(number, text)
ok  -> MessageDialog(ui, "Message sent!").show(); return true
else-> MessageDialog(ui, "Send failed: %s" % detail).show(); return false
```

`len(text)` is a **Unicode code-point count** in Python. In C, count UTF-8 code
points, not bytes, or a 160-character message with any non-ASCII in it is
rejected when it should not be.

**`_show_write_message(ui, root_id=2, sub_index=3)`:**

```
input_widget = TextInputLong(ui, "Write")        # no initial text
cursor_on = true; last_blink = now()
input_widget.draw(cursor_on); softkey.update("Options")
loop:
    if now() - last_blink > 0.5:
        cursor_on = !cursor_on; last_blink = now()
        input_widget.draw(cursor_on); softkey.update("Options")
    key = wait_for_key(); if none: continue
    if key in (28, 96):
        VerticalList(ui, "Options", ["Send", "Save"], app_id="2-3").show()
        sel == 0 -> if _send_message_flow(ui, get_text(), 2, 3): return   # sent -> leave
                    # failed/cancelled: keep the draft on screen
        sel == 1 -> _save_outbox_message(get_text()); MessageDialog(ui, "Saved!").show()
        input_widget.draw(cursor_on); softkey.update("Options"); continue
    result = input_widget.handle_key(key)
    if result == "empty_backspace": return       # Back on an empty field exits
    input_widget.draw(cursor_on); softkey.update("Options")
```

**Dead code to *not* port:** `_show_stub_screen(ui, title, root_id, sub_index)`
(lines 32–51) has no callers anywhere in the tree.

#### Public entry points the core calls

```
open_message(ui, message_id)   # notify banner, exactly one unread SMS
    row = SELECT ... FROM inbox WHERE id = message_id
    if row is none: return _show_inbox(ui, 2, 1)
    _mark_read(row.id)
    _show_message_detail(ui, "Inbox", "2-1", 1, row.message,
                         message_id=row.id, sender=row.sender, timestamp=row.timestamp)

open_inbox(ui)                 # notify banner, several unread
    _show_inbox(ui, 2, 1)
```

Today `core/main.py:_open_notification` imports `Messages/main.py` **into the
core process** and calls these. With process-per-app the core must instead
`fork`+`exec` `nd-apprun` with an argument selecting the entry point. See
"Public interface" below.

### 8. Settings (`apps/Settings/main.py`, 310 lines)

```c
#define SETTINGS_ROOT_ID       4
#define SYSTEM_WALLPAPER_DIR   "/NeoDCT/System/wallpapers"
#define WALLPAPER_DIR          "/NeoDCT/User/wallpapers"
#define SDCARD_HELPER          "/NeoDCT/System/hw/neodct-sdcard"
#define ENGINEERING_MODE_KEY   "system.ui.engineering_mode"
#define GET_MORE_LABEL         "Get more..."
static const char *SUPPORTED_WALLPAPERS[2] = {".jpg", ".jpeg"};
```

`run(ui)` loop: `VerticalList(ui, "Settings", ["Wallpaper", "Memory card",
"Engineering Mode", "About"], app_id=4)` built **inside** the loop;
`SoftKeyBar(ui).update("Select", present=False)`; `-1` exits; 0/1/2/3 →
`_show_wallpaper_menu` / `_show_memory_card` / `_show_engineering_mode` /
`_show_about`. Golden frame: `app-settings.png`.

#### 8a. Wallpaper

`_wallpaper_dirs()` = `Storage.media_dirs("wallpapers",
system_dir=SYSTEM_WALLPAPER_DIR)` (existing directories only, stock first, then
the card's `wallpapers/`), then append `WALLPAPER_DIR` if it is a directory and
not already present.

`_scan_wallpapers()`: for each dir, `os.walk` and for each `sorted(files)` whose
lowercased name ends `.jpg`/`.jpeg`, record
`{"name": basename-without-extension, "path": full_path}`. Then a **global**
`sort(key=name.lower())`.

`_wallpaper_menu_once(ui)`:

```
try os.makedirs(WALLPAPER_DIR, exist_ok=True)   # swallow failure (read-only rootfs)
wallpapers = _scan_wallpapers()
insert at 0:  {"name": "None",         "path": "NONE"}
append:       {"name": "Get more...",  "path": None}
vlist = VerticalList(ui, "Wallpaper", [names], app_id=4)
SoftKeyBar(ui).update("Select", present=False)
sel = vlist.show(); if -1: return NONE
if selected.path is None:
    TextScroller(ui, Storage.is_ready() ? GET_MORE_HELP_WITH_CARD : GET_MORE_HELP).show()
    return "again"                              # _show_wallpaper_menu re-opens the list
set_setting("system.ui.wallpaper", selected.path)
if path == "NONE": ui.wallpaper = None
else:              ui.wallpaper = ui.load_wallpaper(path)
MessageDialog(ui, "Wallpaper set to\n%s" % selected.name).show()
```

`_show_wallpaper_menu(ui)` loops `_wallpaper_menu_once` while it returns
`"again"`.

**Shipped wallpapers** (in the read-only image, `/NeoDCT/System/wallpapers/`):
`90s throwback.jpg`, `Dark Fantasy.jpg`, `Digital Swamp.jpg`,
`Fruitiger Aero.jpg`, `Grasslands.jpg`, `Palestine.jpg`. With no card the list is
therefore exactly:

```
None | 90s throwback | Dark Fantasy | Digital Swamp | Fruitiger Aero | Grasslands | Palestine | Get more...
```

Golden frame `app-settings-wallpaper.png` shows the first three rows of that.

**Help strings, verbatim** (embedded `\n`s are real; note the escaped quotes):

```
GET_MORE_HELP =
"Get more wallpapers by adding an SD card!\n"
"\n"
"Format a card as FAT32, make a folder called \"wallpapers\" on it, and copy your .jpg files into it.\n"
"\n"
"240x240 pictures look best. Put the card in the phone and they appear in this list. The phone can set a blank card up for you."

GET_MORE_HELP_WITH_CARD =
"Get more wallpapers from your SD card!\n"
"\n"
"Copy .jpg files into the \"wallpapers\" folder on the card that is in the phone and they appear in this list. 240x240 looks best."
```

#### 8b. Memory card

`_show_memory_card(ui)` branches on `Storage.card().state`:

```
"absent":
    MessageDialog(ui, "No memory card.", button_text="More").show()
    TextScroller(ui, SDCARD_HELP).show(); return
"ready":
    MessageDialog(ui, "Memory card ready.\n%s on %s"
                      % (card.label or "unnamed", card.device or "card"),
                  button_text="More").show()
    TextScroller(ui, SDCARD_HELP).show(); return
"needs_setup":
    if MessageDialog(ui, "This card is not set up for NeoDCT.\nAdd the NeoDCT folders to it?",
                     button_text="Set up").show() != 28: return
    if Storage.setup_folders():
        MessageDialog(ui, "Card is ready to use.", button_text="OK").show()
    else:
        MessageDialog(ui, "Could not write to the card.\nIt may be locked or damaged.",
                      button_text="OK", cancel_keys=()).show()
    return
otherwise ("unformatted"):
    if MessageDialog(ui, "This card cannot be read.\nFormat it? EVERYTHING ON IT WILL BE ERASED!",
                     button_text="Format").show() != 28: return
    if not card.device:
        MessageDialog(ui, "No card device to format.", button_text="OK",
                      cancel_keys=()).show(); return
    rc = subprocess.call([SDCARD_HELPER, "format", card.device])     # blocking
    rc == 0 -> MessageDialog(ui, "Card formatted and ready.", button_text="OK").show()
    else    -> MessageDialog(ui, "Formatting failed.\nThe card may be write protected.",
                             button_text="OK", cancel_keys=()).show()
```

`cancel_keys=()` on three of those dialogs means **only key 28 dismisses them** —
14 is ignored. Reproduce.

`SDCARD_HELP`, verbatim (the two-space indents in the folder list are
significant):

```
"A NeoDCT memory card is a FAT32 card with these folders on it:\n"
"\n"
"  wallpapers   .jpg pictures\n"
"  tones        .mp3 ringtones\n"
"  music        your music\n"
"  backup_db    copies of your contacts\n"
"  update       UPDATE.ndsw system updates\n"
"\n"
"You can make one on a computer, or let the phone do it. Setting up only adds the folders. Formatting erases everything on the card."
```

Covered exhaustively by `test_settings_memory_card.py` (9 tests).

#### 8c. Engineering Mode

```
current = _setting_is_enabled(get_setting("system.ui.engineering_mode", "ON"), default=True)
menu = VerticalList(ui, "Eng. Mode", ["On", "Off"], app_id=4)
menu.selected_index = current ? 0 : 1
SoftKeyBar(ui).update("Select", present=False)
sel = menu.show(); if -1: return
enabled = (sel == 0)
set_setting("system.ui.engineering_mode", enabled ? "ON" : "OFF")
_refresh_engineering_apps(ui, enabled)
MessageDialog(ui, "Engineering Mode set to %s." % (enabled ? "ON" : "OFF")).show()
```

`_setting_is_enabled(value, default=True)`: `None` → default; lowercase-stripped
in `{"1","true","on","yes","enabled"}` → true; in
`{"0","false","off","no","disabled"}` → false; anything else → default.

`_refresh_engineering_apps(ui, enabled)` **mutates the launcher's live state**:

```
if not hasattr(ui, "apps"): return
ui.engineering_mode = bool(enabled)                       (exceptions swallowed)
ui.apps = [app for app in ui.apps if "/NeoDCT/System/engineering/apps/" not in app["path"]]
if enabled and hasattr(ui, "_scan_apps_from_dir"):
    ui._scan_apps_from_dir("/NeoDCT/System/engineering/apps")
ui.apps.sort(key=lambda item: item["id"])                 (exceptions swallowed)
```

**This cannot work across a process boundary.** See "Behaviour that changes"
below.

#### 8d. About

```
title = "NeoDCT"
version_name   = get_setting("system.os.versionname",   "NeoDCT OS")
version_number = get_setting("system.os.versionnumber", "")
build_time     = get_setting("system.os.buildtime",     "Unknown")
if not build_time or upper(build_time) == "NONE": build_time = "Unknown"

draw.rectangle((0, 0, 240, 175), fill="black")            # FULL screen
(w, _) = get_text_size("NeoDCT", font_n)
draw.text(((240 - w)/2, 12), "NeoDCT", font=font_n, fill="white")
line_pad = max(10, int(240 * 0.12)) = 28
draw.line((28, 30, 212, 30), fill="white")
y = 42
if version_name:
    for line in _wrap_text(ui, version_name, 220, font_s)[:2]:
        if y > 127: break
        (w,_) = get_text_size(line, font_s)
        draw.text(((240 - w)/2, y), line, font=font_s, fill="white"); y += 16
    y += 6
if version_number:
    if y <= 127: draw.text((10, y), "Version: " + version_number, font=font_s, fill="gray")
    y += 16                                # NOTE: y advances even when the text was skipped
if y <= 127: draw.text((10, y), "Build time:", font=font_s, fill="gray")
y += 16
for line in _wrap_text(ui, build_time, 220, font_s)[:2]:
    if y > 127: break
    draw.text((10, y), line, font=font_s, fill="gray"); y += 16
SoftKeyBar(ui).update("Back", present=False)
fb.update(canvas)
wait until key == 14                       # ONLY 14 exits
```

Settings has its **own** `_wrap_text(ui, text, max_width, font)`, simpler than
the Messages one: split on whitespace, greedy fill, **no `...` handling** for a
single over-long word (it is emitted on its own line and overflows).

Defaults from `SettingsStorage.DEFAULTS`: `system.os.versionnumber` `"0.3.1a"`,
`system.os.versionname` `"NeoDCT System v0.3.1a"`. `system.os.buildtime` has no
default, so it comes from `/NeoDCT/System/version.prop` or reads `"Unknown"`.

### 9. Tones (`apps/Tones/main.py`, 229 lines)

```c
#define TONES_ROOT_ID     9
#define SYSTEM_TONES_DIR  "/NeoDCT/System/tones"
#define USER_TONES_DIR    "/NeoDCT/User/tones"
#define ADD_MORE_LABEL    "Add more..."
/* SUPPORTED_EXTS = (".mp3")  -- a bare STRING in the Python, not a 1-tuple.
   endswith() accepts a str, so behaviour is "ends with .mp3". Keep that. */
static const char *MPV_CMD[] = {"nice","-n","-10","mpv","--no-video",
                                "--audio-buffer=4","--quiet", /* path */ NULL};
```

`run(ui)` loop: `PagedList(ui, "Tones", ["Ringing Options", "Ringing Tones"],
root_id=9)` built **inside** the loop; `-1` exits; 0 → `_show_ringing_options`;
1 → `_show_ringing_tones`. Golden frame: `app-tones.png`.

`_show_ringing_options(ui)`: `VerticalList(ui, "Ringing Options",
["Ring", "Vibrate"], app_id=9)`, `SoftKeyBar` `"Select"` `present=False`;
`-1` returns; otherwise **either** choice shows
`MessageDialog(ui, "Option saved (no effect yet).")` and nothing is persisted.

`_tone_dirs()` = `Storage.media_dirs("tones", system_dir=SYSTEM_TONES_DIR)` then
append `USER_TONES_DIR` if it is a directory and not already listed.

`_scan_tones()`: skip non-existent bases; `os.walk`; `sorted(files)`; keep names
whose lowercase ends `.mp3`; entry `{"name": basename-without-extension,
"path": full}`; then a global `sort(key=name.lower())`.

`/NeoDCT/System/tones/` also contains `sms.wav` and a `dtmf/` subdirectory of
twelve `.wav` files — the `.mp3` filter excludes all of them even though
`os.walk` descends into `dtmf/`. The 16 stock tones are:
`Auld Lang Syne, Badinerie, Basic Rock, Bossanova, Brave Scotland, Bumblebee,
Drip Groove, Entertainer, Groovy Blue, Jingle Bells, Low, Mexican Hat Dance,
Nokia Tune, Ring Ring, Tchaikovsky, Valkyrie`.

**`_show_ringing_tones(ui)` — a hand-rolled list loop with debounced preview.**
This is the only app that drives a `VerticalList` manually instead of calling
`show()`:

```
try os.makedirs(USER_TONES_DIR, exist_ok=True)      # swallow failure
tones = _scan_tones()
if empty: MessageDialog(ui, "No ringtones found.").show(); return
tones.append({"name": "Add more...", "path": None})
names = [t.name for t in tones]
vlist = VerticalList(ui, "Tones", names, app_id=9)
softkey = SoftKeyBar(ui)
player = TonePreviewPlayer()
pending_index = none; pending_time = 0.0

schedule_preview():                     # called on every selection change
    player.stop()
    if tones[vlist.selected_index].path is None: pending_index = none; return
    pending_index = vlist.selected_index; pending_time = now()

redraw(): softkey.update("Select", present=False); vlist.draw()

_flush_input(ui); redraw()
loop:
    if pending_index is not none and now() - pending_time >= 0.5:
        player.play(tones[pending_index].path); pending_index = none
    key = ui.read_keypress(0.05)
    if key is none: continue
    108 DOWN:  if selected_index < len(names)-1:
                   selected_index++
                   if selected_index >= window_start + max_lines: window_start++
                   schedule_preview(); redraw()
    103 UP:    if selected_index > 0:
                   selected_index--
                   if selected_index < window_start: window_start--
                   schedule_preview(); redraw()
    2..10:     i = key - 2
               if i < len(names):
                   selected_index = i
                   if selected_index < window_start: window_start = selected_index
                   elif selected_index >= window_start + max_lines:
                        window_start = max(0, selected_index - max_lines + 1)
                   schedule_preview(); redraw()
    28 or 96:  player.stop()
               if tones[selected_index].path is None:
                   TextScroller(ui, Storage.is_ready() ? ADD_MORE_HELP_WITH_CARD
                                                       : ADD_MORE_HELP).show()
                   _flush_input(ui); redraw(); continue
               set_setting("system.audio.ringtone", tones[selected_index].path)
               MessageDialog(ui, "Ringtone set to %s." % names[selected_index]).show()
               return
    14:        player.stop(); return
```

Note the digit shortcut range is `2 <= key <= 10`, i.e. digits **1–9**; the
`0` key (11) is not a shortcut. `vlist.max_lines` is 3 once `draw()` has run
(the framework computes it), so the C port must call the equivalent of `draw()`
before relying on it.

`_flush_input(ui)`: while `select([keypad_fd], [], [], 0.01)` is readable,
`os.read(fd, 24)`; stop on any error or when not readable. No-op when
`keypad_fd` is `None`.

`TonePreviewPlayer`:

```
play(path): if not path: return
            stop()
            Popen(MPV_CMD + [path], stdout=DEVNULL, stderr=DEVNULL)
            on exception: print "[Tones] Failed to play {path}: {exc}"; process = none
stop():     if no process: return
            terminate() (SIGTERM); wait(timeout=0.2)
            on any failure: kill() (SIGKILL), errors swallowed
            process = none
```

**Help strings, verbatim:**

```
ADD_MORE_HELP =
"Add more ringtones by adding an SD card!\n"
"\n"
"Format a card as FAT32, make a folder called \"tones\" on it, and copy your .mp3 files into it.\n"
"\n"
"Put the card in the phone and the tones appear in this list next to the built-in ones. The phone can set a blank card up for you from Settings."

ADD_MORE_HELP_WITH_CARD =
"Add more ringtones from your SD card!\n"
"\n"
"Copy .mp3 files into the \"tones\" folder on the card that is in the phone, and they appear in this list next to the built-in ones."
```

### 10. MusicPlayer (`apps/MusicPlayer/main.py`, 516 lines)

```c
#define MUSIC_FOLDER "music"          /* a folder name on the SD card, not a path */
#define MINIAUDIO_RATE 44100
/* env overrides */
"NEODCT_MUSIC_AUDIO"    /* "subprocess" forces mpv; "miniaudio" makes absence fatal */
"NEODCT_MUSIC_ABUF_MS"  /* default "500" */
static const char *MPV_CMD[] = {"nice","-n","-10","mpv","--no-video",
                                "--audio-buffer=4","--quiet", /* path */ NULL};
```

`ImageFile.LOAD_TRUNCATED_IMAGES = True` is set at import — the PNG/JPEG decoder
in C must likewise tolerate a truncated embedded APIC image rather than failing.

#### 10a. Entry point

```
run(ui):
    app = MusicPlayer(ui)
    if app.music_dir() is None:                 # Storage.folder("music") -> no ready card
        MessageDialog(ui, "No SD card.\nMusic is played from a card.",
                      button_text="More").show()
        TextScroller(ui, NO_CARD_HELP).show()
        return
    try: app.run()
    finally: app.stop()
```

Golden frame `app-musicplayer.png` is exactly that `MessageDialog` (there is no
card in the harness).

`NO_CARD_HELP`, verbatim:

```
"Music is played from an SD card.\n"
"\n"
"Format a card as FAT32, make a folder called \"music\" on it, and copy your .mp3, .flac, .wav or .ogg files into it.\n"
"\n"
"Put the card in the phone and your music shows up here. The phone can set a blank card up for you from Settings."
```

#### 10b. Backend selection

```
_pick_player():
    forced = getenv("NEODCT_MUSIC_AUDIO", "")
    if forced != "subprocess" and HAS_MINIAUDIO:
        print "[Music] audio: in-process miniaudio streaming"; return _MiniaudioPlayer()
    if forced == "miniaudio":
        print "[Music] NEODCT_MUSIC_AUDIO=miniaudio but module missing; audio disabled"; return NULL
    print "[Music] audio: external mpv processes"; return _MpvPlayer()
```

`_MiniaudioPlayer.EXTS = (".mp3", ".wav", ".flac", ".ogg")`;
`_MpvPlayer.EXTS = (".mp3", ".wav", ".aac", ".flac", ".ogg")` — the extension
filter for the track list depends on which backend is live.

`_MiniaudioPlayer`:

```
play(path):
    stop(); _ended = false; _frames = 0
    buf_ms = int(getenv("NEODCT_MUSIC_ABUF_MS", "500"))
    frames_to_read = max(16384, 44100 * buf_ms * 2 / 1000)     /* 44100 at 500 ms */
    stream = miniaudio.stream_file(path, SIGNED16, nchannels=2,
                                   sample_rate=44100, frames_to_read=frames_to_read)
    wrapped = stream_with_callbacks(stream, progress_callback=_on_progress,
                                    end_callback=_on_end)
    next(wrapped)                                              /* prime the generator */
    device = PlaybackDevice(SIGNED16, nchannels=2, sample_rate=44100,
                            buffersize_msec=buf_ms, app_name="NeoDCT Music")
    device.start(wrapped); is_paused = false
_on_progress(n): _frames += n
_on_end():       _ended = true
stop():          device.close() (errors swallowed); device = stream = none; is_paused = false
toggle_pause():  if no device or _ended: return
                 is_paused ? (device.start(stream); is_paused = false)
                           : (device.stop();        is_paused = true)
is_finished():   device is none or _ended
position():      _frames / 44100.0                             /* seconds */
```

`_MpvPlayer`:

```
play(path):     stop(); Popen(MPV_CMD + [path], DEVNULL, DEVNULL); is_paused = false
stop():         terminate(); wait(0.2); on failure kill(); process = none; is_paused = false
toggle_pause(): if no process: return
                try: is_paused ? kill(pid, SIGCONT) : kill(pid, SIGSTOP); flip is_paused
                except (ProcessLookupError, OSError): is_paused = false
is_finished():  process is none or poll() is not none
position():     None                                           /* wall-clock fallback used */
```

#### 10c. Scanning

```
music_dir() = Storage.folder("music")        # "/NeoDCT/User/sdcard/music" or None
scan_music():
    playlist = []
    exts = player.EXTS if player else ()
    if exts and music_dir and exists(music_dir):
        for (root, dirs, files) in os.walk(music_dir):
            for f in sorted(files):
                if lower(f) ends with any of exts: playlist.append(root + "/" + f)
```

**Not** globally sorted — sorted per directory, in `os.walk` order.

#### 10d. Metadata

```
get_metadata(path) -> {title, artist, album, art, length}
  defaults: title = basename(path), artist = "Unknown Artist", album = "",
            art = None, length = 0
  if mutagen available:
      audio = MP3(path, ID3=ID3)                     # mp3 only
      if audio.tags:
          "TIT2" -> title ; "TPE1" -> artist ; "TALB" -> album   (str() of the frame)
          first APIC frame -> art = Image.open(BytesIO(frame.data)); on failure art = None
      length = audio.info.length                     # seconds, float
      on any exception: print "[Music] Metadata error: {e}"
  if length == 0 and miniaudio available:
      try length = miniaudio.get_file_info(path).duration   (swallow failure)
  if art is None: art = find_folder_art(path)
```

`find_folder_art(path)`: list the containing directory into a
lowercase→real-name map; for `name` in `("cover","folder","front","album",
"albumart")` × `ext` in `(".jpg",".jpeg",".png")`, in that nested order, open the
first match; `OSError` on the listing → `None`.

`format_time(seconds)` = `"%02d:%02d" % (seconds // 60, seconds % 60)` on an
**int** — durations over 99 minutes print three digits for the minutes.

#### 10e. `play_file(path)` and the miniaudio→mpv fallback

```
if player is none: return false
try player.play(path); return true
except e:
    print "[Music] playback failed: {type(e).__name__}: {e}"
    if HAS_MINIAUDIO and player is a _MiniaudioPlayer
       and e is NOT a miniaudio.DecodeError
       and getenv("NEODCT_MUSIC_AUDIO","") != "miniaudio":
        print "[Music] falling back to mpv"
        player = _MpvPlayer(); return play_file(path)     # one retry
    return false
```

A decode error means the *file* is bad; anything else (usually device init) means
the in-process path is unusable for the whole session.

#### 10f. The track list (`run()`)

```
loop:
    scan_music()
    if playlist empty:
        draw.rectangle((0, 0, 240, 145), fill="black")
        y = max(12, int(145 * 0.35)) = 50
        draw.text((10, 50),  "No Music Found", font=font_n, fill="white")
        draw.text((10, 80),  "Add mp3s to:",   font=font_s, fill="gray")
        draw.text((10, 100), "/User/music",    font=font_s, fill="gray")
        softkey.update("Exit")
        wait until key in (14, 28); return
    display_list = [basename(p) for p in playlist]
    vlist = VerticalList(ui, "Music", display_list, app_id=4)     # NOTE: 4, not 970
    sel = vlist.show(); if sel == -1: return
    if play_file(playlist[sel]): run_now_playing(playlist[sel])
```

`app_id=4` collides with Settings' root id, so the breadcrumb reads `4-N`. This
is a bug; **port it as-is** and note it (`CODING-STANDARDS.md` §9.4).
The `"/User/music"` hint is also stale — music lives on the card at
`/NeoDCT/User/sdcard/music`.

#### 10g. Now Playing — exact geometry

Computed values on 240×175 (all verified numerically):

```
header_h            = max(24, int(175 * 0.08))          = 24
art_size (initial)  = min(100, max(64, int(240 * 0.42)))= 100
art_x               = 8
art_y               = header_h + 12                     = 36
text_x              = art_x + art_size + 8              = 116   <-- uses the PRE-SHRINK size
text_width          = max(30, 240 - text_x - 8)         = 116
bar_width           = max(48, 240 - 20)                 = 220
bar_x               = (240 - bar_width) / 2             = 10
bar_y               = content_bottom - 18               = 127
available_media_h   = max(48, bar_y - art_y - 18)       = 73
art_size (final)    = min(art_size, available_media_h)  = 73
```

**Quirk:** `text_x` and `text_width` are computed **before** `art_size` is
reduced from 100 to 73, so there is a 27-px gap between the art and the text
column. Reproduce.

Sequence:

1. Loading card:
   `draw.rectangle((0,0,240,175), fill="black")`; `"Loading..."` in `font_n`
   centred at `((240-lw)/2, max(10, (145-lh)/2))`; `fb.update`.
2. `meta = get_metadata(path)`.
3. If `meta.art`: `art.draft("RGB", (200, 200))`, `art.load()`,
   `display_art = art.resize((100, 100), NEAREST)`; any exception →
   `display_art = None`. Then, after `art_size` drops to 73, if
   `display_art.size != (73, 73)`: `display_art = display_art.resize((73,73), NEAREST)`.
   **`Image.Resampling.NEAREST`, not LANCZOS** — the resampling filter is visible.
   `draft()` is a JPEG-only DCT-scaling hint; a C decoder can use libjpeg's
   `scale_num`/`scale_denom` for the same effect, or skip it (it only affects
   speed and memory, not the final NEAREST-resampled pixels, for scale factors
   the draft path does not reach).
4. Clock bookkeeping: `start_time = now()`, `paused_at = 0`,
   `total_paused_duration = 0`.
5. Render loop:

```
if player.is_finished(): stop(); return

now = time()
if player.is_paused:
    if paused_at == 0: paused_at = now
    current_elapsed = paused_at - start_time - total_paused_duration
else:
    if paused_at != 0: total_paused_duration += (now - paused_at); paused_at = 0
    current_elapsed = now - start_time - total_paused_duration
pos = player.position()
if pos is not none: current_elapsed = pos        # miniaudio overrides the wall clock

if needs_redraw:
    draw.rectangle((0, 0, 240, 145), fill="black")

    /* header */
    draw.rectangle((0, 0, 240, 24), fill="white")
    (w,h) = get_text_size("Now Playing", font_s)
    draw.text(((240-w)/2, max(2, (24-h)/2)), "Now Playing", font=font_s, fill="black")

    /* album art at (8, 36), 73x73 */
    if display_art:
        canvas.paste(display_art, (8, 36))                       /* NO alpha mask */
        draw.rectangle((7, 35, 81, 109), outline="white")
    else:
        draw.rectangle((8, 36, 81, 109), outline="white")
        cx = 8 + 73/2 = 44 ; cy = 36 + 73/2 = 72
        draw.ellipse((36, 80, 45, 89), fill="white")             /* note head   */
        draw.line((45, 84, 45, 60), fill="white", width=2)       /* stem        */
        draw.line((45, 60, 58, 64), fill="white", width=2)       /* flag        */

    /* right column, truncate() = drop chars until text+"..." fits 116 px */
    draw.text((116, 36),  truncate(title,  font_n, 116), font=font_n, fill="white")
    draw.text((116, 61),  truncate(artist, font_s, 116), font=font_s, fill="#cccccc")
    if album:
        draw.text((116, 81), truncate(album, font_s, 116), font=font_s, fill="#999999")

    /* progress */
    draw.rectangle((10, 127, 230, 131), fill="#333333")
    pct = (length > 0) ? min(1.0, current_elapsed / length) : 0
    draw.rectangle((10, 127, 10 + (int)(220 * pct), 131), fill="white")

    /* timestamps at y = 112 */
    draw.text((10, 112), format_time((int)current_elapsed), font=font_s, fill="white")
    if length > 0:
        total = "-" + format_time((int)max(0, length - current_elapsed))
        (w,_) = get_text_size(total, font_s)
        draw.text((230 - w, 112), total, font=font_s, fill="white")

    softkey.update(player.is_paused ? "Play" : "Pause")
    needs_redraw = false

key = ui.read_keypress(1.0)
if key is none: needs_redraw = true; continue      /* repaint once a second */
needs_redraw = true
14 -> stop(); return
28 -> player.toggle_pause()
```

`truncate(text, font, max_w)` is the app's own:

```
t = text; (w,_) = get_text_size(t, font)
while w > max_w and len(t) > 0:
    t = t[:-1]; (w,_) = get_text_size(t + "...", font)
return (len(t) < len(text)) ? t + "..." : t
```

Note it measures `t` on the first pass and `t + "..."` on every pass after —
an off-by-one in the original. Port it exactly.

### 11. Browser (`apps/Browser/main.py`, 261 lines)

> **This conflicts with `ARCHITECTURE.md`.** That document says the browser is
> "WebKitGTK running under the `cage` compositor". It is not. It is
> **`/usr/bin/netsurf-fb`** — NetSurf's framebuffer front-end, drawing straight
> to `/dev/fb0`. Both defconfigs set `BR2_PACKAGE_NETSURF=y` and
> `BR2_PACKAGE_NETSURF_FRAMEBUFFER=y`, and there is no cage or WebKit package in
> either. The conclusion ("out of scope, nothing changes") still holds, but the
> mechanism is different and the *wrapper* around it does need porting.

```c
#define BROWSER_BIN "/usr/bin/netsurf-fb"
#define HOME_PAGE   "file:///NeoDCT/System/apps/Browser/home.html"
#define CONSOLE     "/dev/console"
#define TAG         "Browser"
```

`run(ui)`:

```
if not exists("/usr/bin/netsurf-fb"): return           # silently, no screen
bridge = _start_key_bridge(ui)
env = copy of environ, with HOME defaulted to "/NeoDCT/User"
stderr_sink = open("/dev/console", "wb", buffering=0)  or DEVNULL on failure
proc = Popen([browser, HOME_PAGE], env=env, stdout=DEVNULL, stderr=PIPE)
_log_console(_tagged("neodct-browser: started pid %d" % proc.pid))
if stderr_sink is a real file: _pump_browser_log(proc, stderr_sink)    # blocks to EOF
proc.wait()
_log_console(_tagged(_describe_exit(proc.returncode),
                     196 if returncode != 0 else 141))
if returncode < 0: _dump_dmesg_tail()
finally:
    close stderr_sink
    if bridge: bridge.stop()        (errors swallowed)
    _drain_input(ui)
    ui.fb.update(ui.canvas)         (errors swallowed)   # repaint over netsurf's pixels
```

The whole body is inside a bare `except Exception: pass`.

`_describe_exit(rc)` — **unit-tested, must match byte for byte**:

```
rc == 0  -> "neodct-browser: exited normally"
rc > 0   -> "neodct-browser: exited with code %d" % rc
rc < 0   -> sig = -rc
            6  -> "neodct-browser: KILLED by signal 6 (SIGABRT)"
            9  -> "neodct-browser: KILLED by signal 9 (SIGKILL, possible OOM)"
            11 -> "neodct-browser: KILLED by signal 11 (SIGSEGV)"
            otherwise -> "neodct-browser: KILLED by signal %d" % sig
```

(In C, `waitpid` status: `WIFEXITED` → `WEXITSTATUS` is the `rc > 0` case;
`WIFSIGNALED` → `WTERMSIG` is `sig`.)

`_log_console(text)`: open `/dev/console` unbuffered for writing, write
`text.encode() + b"\r\n"`, close. All errors swallowed. **`\r\n`, not `\n`.**

`_tagged(body, code=141)` = `paint("[Browser]", code, bold=True) + " " + body`,
where `paint` is `System.core.logstyle.paint` (falls back to the plain text if
the import fails). Colour 141 is the purple used for this tag.

`_classify(line)` → `(colour, line)`:

```
lowercase the line
starts with "neodct-mem:"          -> (141, line)
contains any of _ERROR_HINTS       -> (196, line)   /* red */
contains "http://" or "https://"   -> (117, line)   /* pale blue */
otherwise                          -> (141, line)
_ERROR_HINTS = ("ssl","tls","certificate","handshake","verify","error","failed",
                "cannot","refused","timed out","unable","denied","abort")
```

`_pump_browser_log(proc, console)` reads `proc.stderr` line by line to EOF in the
**foreground** (a thread would outlive the process it reads). For each line:
decode UTF-8 with replacement, strip trailing `\r\n`, skip blank lines, classify;
if it starts with `neodct-mem:`, append `" cpu=%.0f%%" % pct` when the sampler
has a reading; write `_tagged(body, code) + "\r\n"` to the console; return on any
write error.

`_CpuSampler(pid).percent()`: read `/proc/<pid>/stat`, split on whitespace,
`busy = int(parts[13]) + int(parts[14])` (utime + stime), `now =
time.monotonic()`. First call stores and returns `None`. Later calls return
`100.0 * (busy - prev_busy) / (SC_CLK_TCK * elapsed)`; `None` when
`elapsed <= 0` or on `OSError`/`ValueError`/`IndexError`.

> **Bug preserved on purpose.** The comment says to index from the closing `)`
> because `comm` may contain spaces, but the code indexes the whitespace split
> at `[13]`/`[14]` regardless. `netsurf-fb` has no space in its name, so it
> works. Port as-is.

`_dump_dmesg_tail(lines=15)`: `subprocess.run(["dmesg"], capture_output=True,
timeout=5)`, take the last 15 lines of stdout, `_log_console` each decoded with
replacement. Errors swallowed.

`_start_key_bridge(ui)`: `System.hw.t9_uinput.start_browser_bridge(ui)`, or
`None` on any exception. Returns `None` when `ui.matrix_input` is `None` (QEMU
has a real keyboard). Owned by the T9/uinput subsystem spec.

`_drain_input(ui)` — **unit-tested**:

```
fd = getattr(ui, "keypad_fd", None)
if fd is not None:
    try: while select([fd], [], [], 0)[0]: if not os.read(fd, 4096): break
    except OSError: pass
matrix = getattr(ui, "matrix_input", None)
if matrix is not None:
    try: for _ in range(64): if matrix.read_key(0) is None: break
    except Exception: pass
```

Note the different read size here (`4096`) versus the 24-byte reads elsewhere.

`Browser/home.html` ships unchanged: a black page titled "NeoDCT Web", a
DuckDuckGo HTML search form (`https://html.duckduckgo.com/html/`, GET, field
`q`) and three link tiles — `https://weather.gov` (Weather),
`https://lite.cnn.com` (News), `https://en.m.wikipedia.org` (Wiki).

---

## Behaviour that changes because apps are now separate processes

Three places in this subsystem cannot survive a literal translation. Each needs
a decision; none of them changes what the user sees.

### C1. Settings mutating the launcher's live state

`Settings/main.py:91,93` set `ui.wallpaper`; `:139,148,151` set
`ui.engineering_mode`, replace `ui.apps` and call `ui._scan_apps_from_dir`.

**Neither change is observable before the app returns.** Inside Settings the
softkey bar is in opaque "app bar" mode, so the wallpaper never shows; the
engineering apps only appear in the `AppSelector`, which the core rebuilds from
`self.apps` on each `render_menu()`.

**Recommended fix (behaviour-identical):** Settings only writes the setting.
After every app exit, the core re-runs:

```
wallpaper_setting = get_setting("system.ui.wallpaper", "NONE")
ui->wallpaper     = (setting is "NONE") ? NULL : nd_ui_load_wallpaper(path)
ui->engineering_mode = _setting_is_enabled(get_setting("system.ui.engineering_mode","ON"))
rescan apps from /NeoDCT/System/apps (+ engineering dir when enabled), sort by id
```

That is cheap (an app exit is already a rescan-worthy event and already re-reads
the unread-SMS count) and needs no IPC. Alternative: an explicit
"settings changed" bit in the child's exit status. **Flagged in
`OPEN-QUESTIONS.md`.**

### C2. `list_ui.show_contact_selector` has three callers in two processes

The core calls it from `handle_input` (home UP/DOWN → dial a contact); PhoneBook
and Messages call it from inside app processes. It must therefore live in
`libneodct.so` (`lib/nd_contacts.c`), not in `apps/PhoneBook/app.so`.

### C3. `Messages.open_message` / `open_inbox` are called *by the core*

`core/main.py:_open_notification` currently imports `Messages/main.py` into the
core process. With process-per-app the core must launch the Messages app with an
entry-point selector. Proposal:

```
execve("/NeoDCT/System/bin/nd-apprun",
       {"nd-apprun", "/NeoDCT/System/apps/Messages", "open_message", "<id>", NULL}, env)
/* or "open_inbox" with no id; default entry is "run" */
```

`nd-apprun` resolves `argv[2]` to an exported symbol (`app_run`,
`app_open_message`, `app_open_inbox`). The app ABI must allow more than one
entry point. **Flagged in `OPEN-QUESTIONS.md`.**

---

## Public interface (the functions other parts call)

### The app ABI these eleven apps need

`ARCHITECTURE.md` and `spec-core-loop.md` name `nd-apprun` and an
`app_shutdown()` teardown hook but do not yet fix the signatures. What this
subsystem requires:

```c
/* apps/<Name>/app.so — exported */
int  app_run(nd_ui *ui);                 /* == Python run(ui); 0 = normal return   */
void app_shutdown(void);                 /* SIGTERM teardown; must be async-safe-ish */

/* Messages additionally: */
int  app_open_message(nd_ui *ui, int64_t message_id);
int  app_open_inbox(nd_ui *ui);
```

Every app in this subsystem returns by falling off the end of `run(ui)`; none
returns a value today.

### `ui` surface the apps actually touch

Counted across all eleven apps. Anything not on this list may be kept private to
the core.

| Read via attribute | Uses | Read via `getattr(..., default)` | Uses |
| --- | ---: | --- | ---: |
| `ui.draw` | 68 | `ui.W` (240) | 10 |
| `ui.font_s` | 22 | `ui.H` (175) | 10 |
| `ui.font_n` | 19 | `ui.SOFTKEY_H` (30) | 10 |
| `ui.get_text_size` | 15 | `ui.content_bottom` (145) | 9 |
| `ui.canvas` | 14 | `ui.keypad_fd` | 2 |
| `ui.fb` (`.update`) | 13 | `ui.modem` | 1 |
| `ui.wait_for_key` | 9 | `ui.matrix_input` | 1 |
| `ui.font_xl` | 7 | | |
| `ui.read_keypress` | 3 | | |
| `ui.font_md` | 1 | | |
| **write / core-only** | | | |
| `ui.wallpaper` (r+w) | 3 | Settings only — see C1 | |
| `ui.apps` (r+w) | 3 | Settings only — see C1 | |
| `ui.engineering_mode` (w) | 2 | Settings only — see C1 | |
| `ui.load_wallpaper()` | 1 | Settings only — see C1 | |
| `ui._scan_apps_from_dir()` | 1 | Settings only — see C1 | |

### Functions exported from this subsystem

| Symbol | Callers | C home |
| --- | --- | --- |
| `run(ui)` (× 11) | core `launch_app` | `apps/*/app.so :: app_run` |
| `Messages.open_message(ui, id)` | core `_open_notification` | `apps/Messages/app.so :: app_open_message` |
| `Messages.open_inbox(ui)` | core `_open_notification` | `apps/Messages/app.so :: app_open_inbox` |
| `list_ui.show_contact_selector(ui, title, btn_text, search_query, header_root)` | core, PhoneBook, Messages | `libneodct :: nd_contacts_pick()` |
| `list_ui.get_all_contacts(search_query)` | `show_contact_selector` | `libneodct :: nd_contacts_query()` |
| `Settings._wallpaper_dirs()`, `_scan_wallpapers()`, `_show_memory_card()`, `SDCARD_HELPER` | **tests** | `apps/Settings/app.c` — keep non-static so the host unit tests can reach them |
| `Tones._tone_dirs()`, `_scan_tones()` | **tests** | `apps/Tones/app.c` — same |
| `Browser._describe_exit()`, `_drain_input()` | **tests** | `apps/Browser/app.c` — same |
| `MusicPlayer.MUSIC_FOLDER` | **tests** | `apps/MusicPlayer/app.c` — same |
| `games_common.{poll_key, get_setting_int, set_setting_value, DIR_KEYS}` | Games, snake, memory | `apps/Games/games_common.h` |

### Libraries these apps consume

| From | Used by |
| --- | --- |
| `libneodct` widgets (`spec-ui-framework.md`) | all eleven |
| `libneodct` `SettingsStorage.get_setting/set_setting` | Settings, Tones, Games, CallLog |
| `libneodct` `Storage.{card, is_ready, folder, setup_folders, media_dirs, FOLDERS, MOUNT_POINT}` | Settings, Tones, MusicPlayer |
| core `ui.modem.send_sms(number, text) -> (ok, detail)` | Messages |
| core `ui.load_wallpaper(path)` | Settings (see C1) |
| `System.core.logstyle.paint(text, code, bold)` | Browser |
| `System.hw.t9_uinput.start_browser_bridge(ui)` | Browser |

---

## External dependencies and their C replacements

| Dependency | Used for | C replacement |
| --- | --- | --- |
| **Pillow — `ui.draw.*`** (`rectangle`, `line`, `text`, `ellipse`, `polygon`) | every app; `polygon` and `ellipse` are used **only** by `memory.py:draw_glyph` and MusicPlayer's note icon | the in-house rasterizer (`nd_draw`). `polygon` needs Pillow's exact scanline fill, including the **self-intersecting** hourglass (glyph 15) |
| **Pillow — `Image.open`** | MusicPlayer album art (from a `BytesIO` of an APIC frame, and from a sidecar file) | `nd_image_load_mem()` / `nd_image_load_file()`; JPEG + PNG only |
| **Pillow — `Image.resize(..., NEAREST)`** | MusicPlayer art, twice | `nd_image_resize_nearest()`. **Not** LANCZOS — visibly different |
| **Pillow — `Image.draft("RGB", (200,200))`** | MusicPlayer, before `load()` | libjpeg `scale_num`/`scale_denom`, or omit (a memory/speed hint only) |
| **Pillow — `canvas.paste(img, box)`** | MusicPlayer art (no mask) | `nd_image_blit()` — straight copy, alpha ignored |
| **`ImageFile.LOAD_TRUNCATED_IMAGES = True`** | MusicPlayer | decoders must return partial data instead of failing |
| **`mutagen`** (`MP3`, `ID3`, `APIC`, `TIT2`, `TPE1`, `TALB`, `audio.info.length`) | MusicPlayer metadata | **write `music_meta.c`** — an ID3v2.2/2.3/2.4 reader for exactly four frames plus an MPEG frame-header duration scan. ~350 lines. See Risks §R3 |
| **`miniaudio`** (`stream_file`, `stream_with_callbacks`, `PlaybackDevice`, `SampleFormat.SIGNED16`, `get_file_info`, `DecodeError`) | MusicPlayer playback + duration | **`miniaudio.h` itself** — the Python package is a binding around that single-header C library. `ma_decoder_init_file` + `ma_device` with a data callback reproduces `stream_file` + `PlaybackDevice` exactly; `ma_decoder_get_length_in_pcm_frames` replaces `get_file_info().duration`. Share the build with `nd_ringer.c` (`spec-core-services.md`) |
| **`sqlite3`** | PhoneBook, Messages, CallLog | `libsqlite3` — already in the image, C API directly |
| **`subprocess.Popen`** (mpv × 2, netsurf-fb, poweroff/reboot) | Tones, MusicPlayer, Browser, Power | `posix_spawnp` (or `fork`+immediate `execvp` per `CODING-STANDARDS.md` §1.1). Needs a `waitpid(-1, …, WNOHANG)` reaper for the fire-and-forget cases |
| **`subprocess.call`** (`sync`, `neodct-sdcard format`) | Power, Settings | spawn + blocking `waitpid`; return the exit status |
| **`subprocess.run(capture_output, timeout=5)`** (`dmesg`) | Browser | spawn with a pipe, read with a 5 s deadline (`poll` + monotonic clock), then reap |
| **`subprocess.PIPE` + line reading** | Browser stderr pump | `pipe2(O_CLOEXEC)` + `fdopen` + `getline` on the read end |
| **`os.kill(pid, SIGSTOP/SIGCONT)`** | MusicPlayer mpv pause | `kill(2)`; `ESRCH` must be tolerated |
| **`proc.terminate()` / `.wait(timeout)` / `.kill()`** | Tones, MusicPlayer | `kill(pid, SIGTERM)`, `waitpid` with a deadline, then `SIGKILL` |
| **`select.select`** | Tones `_flush_input`, Browser `_drain_input` | `poll(2)` with the same timeouts (0.01 s and 0) |
| **`os.read(fd, 24)` / `os.read(fd, 4096)`** | input flush/drain | `read(2)`; keep both sizes |
| **`os.walk`, `os.listdir`, `os.makedirs`, `os.path.*`** | Settings, Tones, MusicPlayer | `opendir`/`readdir`/`stat`, `mkdir -p` helper. `os.walk` must be **non-recursive in C** per `CODING-STANDARDS.md` §1.5 — use an explicit stack with a depth cap |
| **`sorted(files)`** | Settings, Tones, MusicPlayer | `qsort` with `strcmp` — Python's default string sort is byte/code-point order, so `strcmp` matches for ASCII filenames |
| **`sort(key=name.lower())`** | Settings, Tones | `qsort` with a `strcasecmp`-style comparator over the *display name*. Python's `str.lower()` is Unicode-aware; ASCII-only in practice here |
| **`time.strftime` / `time.localtime`** | Messages, CallLog | `strftime` / `localtime_r` |
| **`time.time` / `time.sleep` / `time.monotonic`** | everywhere | `clock_gettime(CLOCK_REALTIME/MONOTONIC)`, `nanosleep`. **The golden-frame harness patches all of these** — the C build's frame dumper needs the same virtual-clock hook |
| **`random.seed/choice/shuffle`** | snake, memory | see Risks §R1 |
| **`io.BytesIO`** | MusicPlayer APIC → `Image.open` | a `(ptr, len)` pair into `nd_image_load_mem()` |
| **`os.sysconf("SC_CLK_TCK")`** | Browser CPU sampler | `sysconf(_SC_CLK_TCK)` |
| **`os.environ` / `env.setdefault`** | MusicPlayer, Browser | `getenv`; build the child `envp` explicitly |
| **`json`** (manifests) | read by the **core**, not the apps | core's manifest parser |
| **`importlib`** (`Games`/`Power` `sys.path` juggling) | Python packaging only | nothing |

**Binaries that must stay in the image:** `mpv` (`BR2_PACKAGE_MPV=y` in both
defconfigs), `netsurf-fb` (`BR2_PACKAGE_NETSURF_FRAMEBUFFER=y` in both),
`/NeoDCT/System/hw/neodct-sdcard` (a shipped busybox-ash script), `dmesg`,
`sync`, `poweroff`, `reboot`.

**Buildroot packages that can be dropped once this subsystem is ported:**
`BR2_PACKAGE_PYTHON_MUTAGEN` and `BR2_PACKAGE_PYTHON_MINIAUDIO` (once
`NotifyService` is also ported — it is the other `miniaudio` user).

---

## Proposed C modules

| File | Contents | est. LOC |
| --- | --- | ---: |
| `apps/Clock/app.c` | the placeholder dialog + double-keypress loop | 40 |
| `apps/Power/app.c` | menu, confirmations, `sync`+spawn ladder, recovery flag | 220 |
| `apps/Calculator/app.c` | `format_number`, `_fold`, `apply_option`, entry editing, draw, loop | 330 |
| `apps/CallLog/app.c` | five screens, timer settings, formatting | 300 |
| `apps/CallLog/calllog_db.c` | schema, `fetch_calls`, `clear_calls` | 120 |
| `apps/Tones/app.c` | two screens, hand-rolled list loop, debounced preview, mpv child | 480 |
| `apps/Browser/app.c` | spawn netsurf, stderr pump, classifier, CPU sampler, drain, repaint | 430 |
| `apps/PhoneBook/app.c` | 7-entry menu, add/edit/erase, contact options, centre messages | 470 |
| `apps/PhoneBook/pb_db.c` | insert / update / delete on `contacts` | 150 |
| `apps/Settings/app.c` | root menu, wallpaper, memory card, engineering mode, About | 700 |
| `apps/Messages/app.c` | root menu, inbox, outbox, detail, compose, send flow, number field | 800 |
| `apps/Messages/msg_db.c` | schema + the seven queries | 250 |
| `apps/Games/app.c` | Games/Snake/Memory menus, `_finish_game`, instruction text | 260 |
| `apps/Games/games_common.c/.h` | key tables, `poll_key`, settings-int helpers | 90 |
| `apps/Games/snake.c` | grid, tick table, step, render, play loop | 260 |
| `apps/Games/memory.c` | board, 20 glyphs, flip/scoring, play loop | 380 |
| `apps/MusicPlayer/app.c` | card check, track list, now-playing screen | 520 |
| `apps/MusicPlayer/music_meta.c` | ID3v2 tag + APIC extraction, MPEG duration scan | 380 |
| `apps/MusicPlayer/music_audio.c` | miniaudio device/decoder wrapper + mpv fallback | 250 |
| `lib/nd_contacts.c` (in `libneodct.so`) | the shared contact selector + `contacts` queries | 200 |
| `lib/nd_pyrandom.c` (in `libneodct.so`) | Python-compatible MT19937 — **only if Risk R1 is answered (a)** | 200 |
| **total** | | **≈ 6,830** |

Build shape (per `ARCHITECTURE.md`): each `apps/<Name>/app.so` links against
`libneodct.so`; `apps/MusicPlayer/app.so` additionally links `miniaudio.h`'s
compiled object (shared with `nd_ringer`) and `libm`; the three database apps
link `libsqlite3`.

---

## Risks

| # | Risk | Severity | Mitigation |
| ---: | --- | --- | --- |
| **R1** | **`game-snake.png` depends on CPython's random number generator.** `SnakeGame.__init__` does `random.seed(time.time())` and `spawn_food()` does `random.choice(open_cells)`. Under the golden harness `time.time()` is the virtual clock, so the result is deterministic — and the reference frame has the food at grid cell **(5,12)**, index 82 of the 403 open cells. Matching that means reproducing MT19937 **plus** CPython's `_Py_HashDouble` float seeding, `init_by_array`, `getrandbits` and `_randbelow`'s rejection sampling. `memory.py`'s `random.shuffle` has the same dependence (invisible in the golden frame, visible in gameplay). | **high** | (a) port `nd_pyrandom.c` — MT19937 is fully specified and CPython's seeding is ~80 lines, total ~200; or (b) give both games an injectable RNG, pin C and Python to the same simple generator, and re-capture `game-snake.png` only. (a) keeps the reference set untouched. **Raise as an open question before writing either.** |
| **R2** | **Settings reaches into the launcher's memory** (`ui.wallpaper`, `ui.apps`, `ui.engineering_mode`, `ui._scan_apps_from_dir`). Impossible across a process boundary. | **high** | C1 above: Settings writes only the setting; the core re-reads wallpaper + engineering mode and rescans apps after every app exit. Behaviour-identical because neither change is visible before the app returns. Confirm with the owner. |
| **R3** | **`mutagen` has no C equivalent.** A naive ID3 reader gets encodings wrong: TIT2/TPE1/TALB can be ISO-8859-1, UTF-16 with BOM, UTF-16BE or UTF-8 depending on the frame's encoding byte, and ID3v2.4 adds sync-safe frame sizes and unsynchronisation. `str(frame)` in mutagen joins multi-value text frames with `"\x00"` removed and returns the first value's text. Getting this wrong shows as mojibake on screen. | **medium** | Write `music_meta.c` against the ID3v2.3 and v2.4 specs; support only TIT2/TPE1/TALB/APIC; handle all four text encodings; ignore extended headers and footers. Unit-test against real files. Fall back to the filename on anything unparsed — exactly what the Python does when `audio.tags` is empty. |
| **R4** | **Duration.** `mutagen.mp3.MP3(...).info.length` scans MPEG frame headers (and Xing/VBRI headers) for a VBR-accurate duration. A naive bitrate×size estimate is wrong on VBR files and the progress bar visibly drifts. | **medium** | For non-MP3 the Python already falls back to `miniaudio.get_file_info(path).duration`; use `ma_decoder_get_length_in_pcm_frames` for **all** formats including MP3, and keep the mutagen path only if a byte-exact match matters. Note it as a deviation if used. |
| **R5** | **Pillow's polygon fill for glyph 15 (hourglass)** is a self-intersecting quadrilateral `[(x0,y0),(x1,y0),(x0,y1),(x1,y1)]`. Its filled shape depends on Pillow's exact scanline/parity rule. Getting it wrong is a visible wrong glyph in Memory. | **medium** | The rasterizer agent owns `nd_draw_polygon`; add this exact 14×14 case to its golden-pixel unit tests. Include glyphs 4, 5, 6, 7, 10, 11, 16, 17 for the same reason. |
| **R6** | **Division by zero.** Python raises `ZeroDivisionError`; C yields inf. The Calculator's error state differs (`acc` and `pending_op` cleared vs left set). | **medium** | Explicit `value == 0.0` test in `_fold`'s `'/'` branch. Covered by a host unit test on `format_number`/`_fold`. |
| **R7** | **Float formatting.** `str(int(v))`, `"%.8f"` + rstrip, `"%.6e"` must match Python byte for byte, including the `-0` case and the 14-character threshold. | **low** | glibc's `printf` matches Python's for `%f`/`%e`. Unit-test the whole `format_number` table, including `1/3`, `1e13`, `-0.0`, `1e-9`. |
| **R8** | **`text_x` uses the pre-shrink art size** in MusicPlayer, leaving a 27-px gap. An agent will "fix" it. | **low** | Called out above; add a comment at the site per `CODING-STANDARDS.md` §9.4. Same for the Clock double-keypress, PhoneBook's non-dialing "Call", MusicPlayer's `app_id=4`, and the two dead PhoneBook menu entries. |
| **R9** | **UTF-8 vs bytes for the 160-character SMS limit and for `TextInputLong`'s character counter.** Python counts code points. | **low** | Count code points in C. One helper in `libneodct`. |
| **R10** | **`os.walk` becomes recursion.** Banned on untrusted input by `CODING-STANDARDS.md` §1.5, and an SD card is untrusted. | **low** | Explicit directory stack, depth cap (say 8), entry cap. |
| **R11** | **Memory: album art.** A 1000×1000 embedded JPEG decodes to ~3 MB of RGB before being resampled to 73×73. On an 8 MB target that is most of the budget. Python already suffers this; `draft()` reduces it for JPEG only. | **medium** | Always use libjpeg's `scale_denom` to decode no larger than ~256 px on the long side before resampling. Refuse APIC payloads over a fixed cap (say 2 MB) and fall through to `find_folder_art`. Note the deviation. |
| **R12** | **Fire-and-forget children are never reaped.** Tones' preview mpv, MusicPlayer's mpv and Power's `poweroff` all `Popen` without a final `wait` on every path. Python's `subprocess` reaps opportunistically; C accumulates zombies. | **low** | One `SIGCHLD`-driven `waitpid(-1, …, WNOHANG)` reaper in each app process (they are short-lived, but Tones can spawn a preview per keypress). |
| **R13** | **The Browser wrapper blocks in `_pump_browser_log` for the whole browsing session.** Any core→app signalling (an incoming call) must still reach it. | **low** | `nd-apprun`'s SIGTERM handler must interrupt the `getline` (do not set `SA_RESTART`) and `app_shutdown()` must kill the netsurf child before exiting. |
| **R14** | **`ARCHITECTURE.md` says cage+WebKitGTK; the code says `netsurf-fb`.** An agent following the architecture doc will delete the wrong thing. | **low** | Fixed in this spec; the architecture doc should be corrected. |

---

## Tests that cover this

Run with `python3 -m pytest neodct/tests/ -q` from the repo root.

### Direct unit tests (29 tests across 5 files, all passing)

| File | Tests | What it pins |
| --- | ---: | --- |
| `neodct/tests/test_apps_sdcard_sources.py` | 10 | `Tones._tone_dirs`, `Tones._scan_tones`, `Settings._wallpaper_dirs`, `Settings._scan_wallpapers`, `MusicPlayer.MUSIC_FOLDER` + `Storage.folder`. Includes "a half-laid-out card contributes nothing" and "the six stock wallpapers really are in the image" |
| `neodct/tests/test_settings_memory_card.py` | 9 | every branch of `Settings._show_memory_card`, including that a mountable card is offered **Set up** (non-destructive) and an unreadable one **Format** with `EVERYTHING ON IT WILL BE ERASED`, that declining runs nothing, and that formatting passes the device to `SDCARD_HELPER` |
| `neodct/tests/test_browser_exit_report.py` | 4 | `_describe_exit` for 0, 1, −6, −9, −11, −31 — exact strings |
| `neodct/tests/test_browser_drain.py` | 4 | `_drain_input` empties the keypad fd and the matrix queue, and tolerates a missing/closed fd |
| `neodct/tests/test_messages_number_field.py` | 2 | `ContactNumberInput.input_filter == "numbers"` and that keys `3,3,42,43` produce `"22*#"` with no multi-tap |

**These are all directly portable** to C host unit tests: none of them draws.
`test_settings_memory_card.py` monkeypatches `MessageDialog` and `TextScroller`
with recorders — in C, make those calls go through function pointers on a small
vtable so the test can swap them, or link a test double.

### Golden frames — the real oracle

`neodct/tests/golden/` holds 49 reference PNGs captured from the Python build by
`neodct/tools/goldenframe.py`, with `manifest.json` recording each frame's size
and the SHA-256 of its **raw RGB bytes**. Determinism is pinned by
`EPOCH = 1704112496.0` (2024-01-01 12:34:56 UTC), `TICK = 0.1` s advanced once
per `fb.update()`, `SEED = 20240101`, and `TZ=UTC`.

**I re-ran the capture during this survey and all fifteen frames below reproduce
byte-identically**, so the oracle is live and trustworthy.

| Frame | App | Key script (evdev) | What it pins |
| --- | --- | --- | --- |
| `app-phonebook` | PhoneBook | `[]` | the 7-entry menu, `1-1` breadcrumb, "Select" softkey |
| `app-messages` | Messages | `[]` | the `PagedList` root, `2-1`, "Inbox" |
| `app-messages-inbox` | Messages | `[28]` | `_show_empty_state`, "No Messages", "Back" |
| `app-calllog` | CallLog | `[]` | the `PagedList` root, `3-1` |
| `app-settings` | Settings | `[]` | the root `VerticalList` |
| `app-settings-wallpaper` | Settings | `[28]` | the wallpaper list with "None" first |
| `app-games` | Games | `[]` | "Memory"/"Snake", `6-1` |
| `app-calculator` | Calculator | `[2, 3, 4]` | "123" right-aligned at `font_xl`, y=12 |
| `app-calculator-options` | Calculator | `[8, 28]` | the Options list over an entry |
| `app-clock` | Clock | `[]` | the warning dialog, centred two-line alert branch |
| `app-tones` | Tones | `[]` | the `PagedList` root, `9-1` |
| `app-musicplayer` | Music | `[]` | the "No SD card." dialog |
| `game-snake` | Games→Snake | `[108, 28, 28]`, budget 300 | board, score, three body cells, **food at (5,12)** |
| `game-memory` | Games→Memory | `[28, 28]`, budget 300 | 40 face-down cards, cursor ring, black inner border |
| `contacts-picker` | shared `list_ui` | direct call | the contact selector over the seeded contact |

Also relevant: `menu-phone-book`, `menu-messages`, `menu-games`,
`menu-settings`, `menu-calculator`, `menu-browser`, `menu-music` (the
`AppSelector` showing each app's icon — they verify the icon files decode
identically, including the three palette PNGs).

**Verification workflow for the C port** (already wired):

```sh
python3 neodct/tools/goldenframe.py --out /tmp/py-frames      # optional re-capture
./build/nd-shoot --out /tmp/c-frames
python3 neodct/tools/goldenframe.py --compare neodct/tests/golden /tmp/c-frames
```

`nd-shoot` must reproduce `shoot_docs.py`'s shot list, the same virtual clock,
and `run_app`'s frame budget / key-script exhaustion semantics.

### Coverage gaps worth adding while porting

Nothing tests, today: `Calculator.format_number` or `_fold` (add a table-driven
host test — cheap and it pins R6/R7); `snake.step` / `queue_turn` / `tick_delay`;
`memory.flip` scoring; `CallLog.format_duration` / `format_call_time`;
`Messages._wrap_text`; `MusicPlayer.format_time` / `truncate`. All are pure
functions and all are one-line ports to a C unit test.

---

## How this could be split across agents

Every app is a separate `.so` with no compile-time dependency on any other, so
this parallelises very well **once the framework exists**. The only shared piece
is `lib/nd_contacts.c`, and only two apps use it.

**Hard prerequisite for all waves:** `libneodct.so` with the rasterizer, the font
engine, and the widgets from `spec-ui-framework.md`; plus `nd_settings`,
`nd_storage` and the app ABI. Nothing here can start before those land.

### Wave 0 — one agent, blocking (≈ 490 LOC)

* `lib/nd_contacts.c` (the shared contact selector — the core needs it too).
* `apps/Games/games_common.c/.h` (Games' three sub-agents all include it).
* Decide R1 and write `lib/nd_pyrandom.c` if the answer is (a).
* Nail down the app ABI (`app_run` / `app_shutdown` / the Messages extra entry
  points) with the core-loop agent.

### Wave 1 — four agents, fully independent, no shared files

| Agent | Apps | LOC | Notes |
| --- | --- | ---: | --- |
| **A1 "small apps"** | Clock, Power, Calculator | 590 | pure widget-driving + one process spawn. Good warm-up; verify `app-clock`, `app-calculator`, `app-calculator-options` |
| **A2 "databases"** | CallLog, PhoneBook | 1,040 | both are SQLite + `VerticalList`/`PagedList`/`InfoScreen`; consumes `nd_contacts` from Wave 0. Verify `app-calllog`, `app-phonebook` |
| **A3 "games"** | Games menus, Snake, Memory | 900 | self-contained; needs `nd_draw_polygon`/`ellipse` and (for R1) `nd_pyrandom`. Verify `app-games`, `game-snake`, `game-memory` |
| **A4 "media sources"** | Settings, Tones | 1,180 | both scan `Storage.media_dirs`, both spawn a helper, both have "Get more…"/"Add more…" `TextScroller` screens — one agent keeps that idiom consistent. Verify `app-settings`, `app-settings-wallpaper`, `app-tones` |

### Wave 2 — three agents, can start with Wave 1 but each has a dependency

| Agent | Apps | LOC | Depends on |
| --- | --- | ---: | --- |
| **A5 "messages"** | Messages | 1,050 | `nd_contacts` (Wave 0), the modem's `send_sms` from `spec-core-services.md`, and the C3 entry-point decision |
| **A6 "music"** | MusicPlayer | 1,150 | the `miniaudio.h` build shared with `nd_ringer` (`spec-core-services.md`), plus JPEG/PNG decode + NEAREST resize from the rasterizer. Biggest single unit — `music_meta.c` (ID3) could be a fifth agent if needed |
| **A7 "browser"** | Browser | 430 | `nd_logstyle` colours and `t9_uinput`'s `start_browser_bridge` from the hw subsystem. Almost no UI — mostly process and pipe plumbing, so it is easy to hand to whoever also does the core's subprocess helpers |

### Sequencing advice

* **A1 first, alone, before anything else in Wave 1.** Clock and Calculator are
  the cheapest end-to-end proof that the app ABI, the framework and the
  golden-frame harness all agree. Three golden frames pass or they do not.
* A3 (games) is the one that can stall on R1. Start it early so the question
  gets answered early.
* A6 (music) is the long pole. If schedule matters, split
  `apps/MusicPlayer/music_meta.c` off as its own agent with a file-based unit
  test and no UI knowledge at all.
* Nobody should touch `neodct/tests/golden/` — it is the reference, not an
  output.

---

## Open questions for the project owner

**Not yet written into `docs/c-rewrite/OPEN-QUESTIONS.md`** — this survey was
read-only outside its own spec file. Copy them across in the format that file
defines.

1. **Snake's food position and Python's RNG (R1).** Reproduce CPython's MT19937
   exactly (~200 LOC in `libneodct`), or give the games an injectable RNG and
   re-capture `game-snake.png`?
2. **Settings changing launcher state (R2/C1).** Confirm that "the core re-reads
   the wallpaper setting, the engineering-mode setting and the app list after
   every app exit" is an acceptable replacement for Settings writing into
   `ui.wallpaper` / `ui.apps` directly.
3. **A second entry point for Messages (C3).** The core's notification "Read"
   softkey calls `Messages.open_message(ui, id)` / `open_inbox(ui)`. Should
   `nd-apprun` take an entry-point name in `argv`, or should Messages read the
   target from an environment variable?
4. **Album-art memory cap (R11).** Is refusing an embedded cover over ~2 MB (and
   falling back to a sidecar `cover.jpg`) acceptable, given the 8 MB target?
5. **MP3 duration (R4).** Is `ma_decoder_get_length_in_pcm_frames` acceptable for
   MP3 as well, or must we reimplement mutagen's Xing/VBRI-aware scan to keep the
   progress bar byte-identical?
6. **`ARCHITECTURE.md` browser correction (R14).** The browser is `netsurf-fb`,
   not cage+WebKitGTK. Confirm the doc should be updated.

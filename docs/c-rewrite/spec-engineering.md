# Engineering menu apps and tools — C port specification

*Survey of `neodct/overlay/NeoDCT/System/engineering/` — 11 apps, 6 tools, 2,634 lines of
Python plus a 59-line busybox script. Written so an agent who has never seen the Python
can reproduce it exactly.*

Cross-references: two of these apps are already specified in depth elsewhere and this
document defers to those rather than contradicting them —
`apps/RemoteShell` in `spec-storage-settings.md` §5 (lines 1238–1290), and
`apps/Downgrade` in `spec-update-system.md` (lines 151, 1698, 1714, 1865). Everything
here about those two is a summary plus the details those specs do not carry.
`spec-hw-input.md` explicitly defers `KeypadMapperI2C` and
`tools/consolei2ckeypadbuilder.py` to **this** document.

---

## What this does (plain English, for a reader who is not a C programmer)

The phone has a hidden set of extra apps. They are not for the person who owns the
phone; they are for whoever is building it. They do things like fill the screen with
solid red to check for dead pixels, print the raw numbers coming out of the battery
chip, show what the modem thinks its signal is, and — the big one — drop you into a real
Linux command prompt on the phone's own screen.

**How they are hidden.** There is a setting called `system.ui.engineering_mode`. It
lives in the same settings file as the wallpaper choice, and out of the box it is
**ON**. When it is on, the phone scans a second folder of apps
(`/NeoDCT/System/engineering/apps`) as well as the normal one, and everything it finds
gets added to the main menu. Turn the setting off in Settings → Engineering Mode and
those apps disappear from the menu immediately — no reboot. It is a visibility switch,
not a password. Anyone who can reach Settings can turn it back on.

**Why they all have huge numbers.** Every app has an `id` in its `manifest.json` and the
menu is sorted by that number. Normal apps use 1–12 (plus Music at 970 and Power at
971). Engineering apps use 999 and 9001–9999, so they always land at the far end of the
menu, after everything a normal user cares about.

**What each one is:**

| App | Menu name | What you do with it |
| --- | --- | --- |
| LCDTest | LCD Test | Fills the screen red, then green, then blue, then a TV-style colour-bar pattern. Press the middle key to step through. It is how you find a dead pixel or a bad ribbon cable. |
| CubeBench | Cube Bench | Spins a wireframe cube and prints frames-per-second. It is a speedometer for the drawing code — the number is the whole point of the app. |
| FuelGauge | FuelGauge | The battery chip (a MAX1704x) is asked for its raw registers once a second and they are printed as-is: voltage, percentage, the chip's own version number. There is a "QStart" softkey that tells the chip to re-guess the charge level from scratch. Refuses to run if the chip is not really there. |
| Modem | ModemInfo | Three pages of live modem facts — signal and network, then SIM identity (IMEI, ICCID, IMSI), then data connection (interface, IPv6 address, APN, DNS). Unlike FuelGauge it deliberately still runs with no modem attached, because seeing *why* there is no modem is the reason you opened it. |
| LinuxShell | Linux Shell | Switches the screen away from the phone UI to a plain Linux text console and runs `/bin/sh`. Type `exit` and the phone UI comes back. On a phone with only a keypad it also starts a "T9 bridge" so you can type letters with the number keys. |
| RemoteShell | Remote Shell | The on/off switch and address book for reaching this phone over the internet with ssh. All the real work is in a core service; this app is seven menu lines. |
| Downgrade | Downgrade | Lists *every* published release of NeoDCT and installs the one you pick, including older ones. The normal Update app only ever offers newer ones. |
| KeypadMapper | KeyMap | Walks you through pressing each of the 16 keypad buttons and writes down which wire crossing each one is, so the phone knows what you pressed. This is the GPIO version. |
| KeypadMapperI2C | KeyMapI2C | The same wizard for the keypad chip that talks over I²C (a PCF8575 chip). This is the one that is actually used. |
| Crash | Crash | One menu item that says "CRASH!". Pressing it deliberately throws an error, so you can check that the crash screen works. |
| TestsApp | Tests | Draws "Hello World" and pops the warning dialog, so you can check those two look right. |

**And a folder of scripts.** `engineering/tools/` holds six small programs that are run
from a command prompt rather than from the menu: send one AT command to the modem
(`atcmd`, which the boot script also uses), watch the battery chip on a bench supply,
build a keymap over the serial cable when the keypad does not work yet, and fill the
contacts and messages databases with fake data for testing.

**Why any of this matters to the rewrite.** Most of it is small and can be ported last.
Two exceptions: `atcmd` is *not* an engineering tool in practice — the modem boot script
calls it on every boot, so it must keep working. And `KeypadMapperI2C` is the only way
to make a fresh phone's keypad work at all, so it cannot be dropped.

---

## Files and where they go in C

### Python source inventory

| Path (relative to `neodct/overlay/NeoDCT/System/engineering/`) | LOC | Purpose |
| --- | ---: | --- |
| `apps/Crash/main.py` | 17 | one-item menu that raises `RuntimeError` |
| `apps/TestsApp/main.py` | 31 | "Hello World" + `MessageDialog` smoke test |
| `apps/LCDTest/main.py` | 64 | RGB flood fills + TV colour-bar pattern |
| `apps/FuelGauge/main.py` | 104 | live MAX1704x register readout, 1 Hz |
| `apps/LinuxShell/main.py` | 120 | VT switch + `/bin/sh -i` on a real tty |
| `apps/CubeBench/main.py` | 144 | rotating wireframe cube + FPS counter |
| `apps/Downgrade/main.py` | 160 | release picker → Update app's installer |
| `apps/RemoteShell/main.py` | 165 | ssh/sftp switch + relay address book |
| `apps/KeypadMapperI2C/main.py` | 273 | PCF8575 keypad enrolment wizard |
| `apps/Modem/main.py` | 296 | 3-page live SIM7600 status |
| `apps/KeypadMapper/main.py` | 303 | gpiozero GPIO keypad enrolment wizard |
| `tools/debug_sms_seed_inbox.py` | 114 | fills `sms_inbox.db` with random messages |
| `tools/mouse_shim.py` | 123 | WASD → uinput absolute-touch cursor for NetSurf |
| `tools/max1704x_watch.py` | 136 | console fuel-gauge watcher / CSV logger |
| `tools/debug_phonebook_createrandomcontacts.py` | 231 | fills `phonebook.db` from a web page |
| `tools/consolei2ckeypadbuilder.py` | 353 | serial-console keymap builder + pin discovery |
| **Python total** | **2,634** | |
| `tools/atcmd` | 59 | **busybox ash** — one AT command, with flock |

### Non-source files that ship with the apps

Every app directory holds exactly three files: `main.py`, `manifest.json`, `icon.png`.
All eleven icons are PNG, 8-bit, colour type 6 (RGBA):

| App | icon.png bytes | dimensions |
| --- | ---: | --- |
| Crash | 11,798 | 120 × 120 |
| CubeBench | 4,775 | 120 × 120 |
| Downgrade | 19,995 | 120 × 120 |
| FuelGauge | 21,443 | 120 × 120 |
| KeypadMapper | 19,881 | 120 × 120 |
| KeypadMapperI2C | 20,654 | 120 × 120 |
| LCDTest | 13,625 | 120 × 120 |
| LinuxShell | 3,089 | **128 × 128** |
| Modem | 14,288 | 120 × 120 |
| RemoteShell | 2,505 | 120 × 120 |
| TestsApp | 6,061 | 120 × 120 |

They are copied byte-for-byte into the image. `AppSelector` loads them through
`ui.get_image(path, max_size=icon_cap)` where `icon_cap` evaluates to **82** on this
geometry (see §0.4), so the cached copy is a LANCZOS thumbnail ≤ 82 px, not the
full-size art.

### Manifest contents (exact bytes matter only for `name`/`id`/`icon`/`exec`)

```
Crash            {"name":"Crash",       "id":"9997", "icon":"icon.png", "exec":"main.py"}
CubeBench        {"name":"Cube Bench",  "id":"9998", "icon":"icon.png", "exec":"main.py"}
Downgrade        {"name":"Downgrade",   "id":"9006", "icon":"icon.png", "exec":"main.py"}
FuelGauge        {"name":"FuelGauge",   "id":"9004", "icon":"icon.png", "exec":"main.py"}
KeypadMapper     {"name":"KeyMap",      "id":"9002", "icon":"icon.png", "exec":"main.py"}
KeypadMapperI2C  {"name":"KeyMapI2C",   "id":"9003", "icon":"icon.png", "exec":"main.py"}
LCDTest          {"name":"LCD Test",    "id":"9001", "icon":"icon.png", "exec":"main.py"}
LinuxShell       {"name":"Linux Shell", "id":"999",  "icon":"icon.png", "exec":"main.py"}
Modem            {"name":"ModemInfo",   "id":"9005", "icon":"icon.png", "exec":"main.py"}
RemoteShell      {"name":"Remote Shell","id":"9990", "icon":"icon.png", "exec":"main.py"}
TestsApp         {"name":"Tests",       "id":"9999", "icon":"icon.png", "exec":"main.py"}
```

`id` is a **JSON string** in the file; `core/main.py:_scan_apps_from_dir` converts it
with `int(data.get("id", 999))` and the menu sorts on that int. `exec` in C becomes
`app.so` (see `ARCHITECTURE.md`); the manifest key stays `"main.py"` or is changed
project-wide — decide once, not per app.

### C destinations

| Python | C destination |
| --- | --- |
| `apps/Crash/main.py` | `apps/Crash/app.c` |
| `apps/TestsApp/main.py` | `apps/TestsApp/app.c` |
| `apps/LCDTest/main.py` | `apps/LCDTest/app.c` |
| `apps/CubeBench/main.py` | `apps/CubeBench/app.c` |
| `apps/FuelGauge/main.py` | `apps/FuelGauge/app.c` |
| `apps/Modem/main.py` | `apps/Modem/app.c` + `apps/Modem/modem_probe.c` (the `/proc`, `/sys`, `/etc` readers) |
| `apps/LinuxShell/main.py` | `apps/LinuxShell/app.c` |
| `apps/RemoteShell/main.py` | `apps/RemoteShell/app_remoteshell.c` (already named by `spec-storage-settings.md`) |
| `apps/Downgrade/main.py` | `apps/Downgrade/app.c` (already named by `spec-update-system.md`) |
| `apps/KeypadMapper/main.py` | `apps/KeypadMapper/app.c` |
| `apps/KeypadMapperI2C/main.py` | `apps/KeypadMapperI2C/app.c` |
| keymap JSON writer, duplicated in both mappers and the console builder | **`libneodct.so` → `nd_keymap_write.c/.h`** |
| `_wrap_text()`, duplicated verbatim in both mappers | **`libneodct.so` → `nd_text_wrap.c/.h`** (the framework already needs it) |
| `tools/atcmd` | **stays busybox ash, byte-for-byte.** `S45modem` calls it by absolute path. |
| `tools/consolei2ckeypadbuilder.py` | `tools/nd-keymap-console` (standalone C binary) |
| `tools/max1704x_watch.py` | `tools/nd-fuelwatch` (standalone C binary) |
| `tools/mouse_shim.py` | `tools/nd-mouse-shim` (standalone C binary) — or dropped, see ranking |
| `tools/debug_sms_seed_inbox.py` | **stays Python**, host/dev only |
| `tools/debug_phonebook_createrandomcontacts.py` | **stays Python**, host/dev only (needs HTTP + an HTML parser) |
| `*/icon.png`, `*/manifest.json` | copied byte-for-byte into the image |

---

## Behaviour that must be reproduced exactly

### 0. The contract every one of these apps assumes

#### 0.1 Geometry and fonts (from `core/main.py` lines 39–43, 534–537, 604–616)

```
ui.W            = 240        UI_WIDTH
ui.H            = 175        UI_HEIGHT      (the letterboxed band; the panel is 240x240)
ui.SOFTKEY_H    =  30        SOFTKEY_HEIGHT
ui.content_bottom = 145      H - SOFTKEY_H

font path       /NeoDCT/System/ui/resources/fonts/font.ttf
ui.font_s  = 14   ui.font_md = 18   ui.font_n = 20   ui.font_xl = 24
```

`ui.get_text_size(text, font)` is **ink extents, not advance width**:

```python
bbox = self.draw.textbbox((0, 0), text, font=font)
return (bbox[2] - bbox[0], bbox[3] - bbox[1])
```

Every centring calculation in this subsystem is built on that, so the C helper must
return the same two numbers. Measured values used below (font.ttf, this exact file):

| Text | Font | (w, h) |
| --- | --- | --- |
| `3D Cube` | s | (68, 13) |
| `FPS 10.0` | s | (68, 13) |
| `BACK/OK to exit` | s | (137, 13) |
| `Ag` | s | (21, 15) |
| `FuelGauge` | xl | (153, 24) |
| `Modem` | xl | (105, 21) |
| `RADIO` / `SIM` / `DATA` | s | (49, 13) / (28, 13) / (44, 13) |
| `i2c-3 @ 0x36` | s | (109, 15) |
| `1/3` | s | (25, 13) |
| `Exit` / `Next` / `QStart` | n | (48, 18) / (58, 18) / (78, 21) |
| `Hello World` | xl | (165, 21) |
| `Keypad Mapper` | n | (193, 21) |
| `Keypad Mapper I2C` | n | (238, 21) |

#### 0.2 Key codes (evdev, unchanged from the kernel)

```
14  BACK / clear (C)      28  ENTER / NaviKey       50  MENU (also 'm')
46  'c'                   96  KP_ENTER             103  UP        108  DOWN
105 LEFT   106 RIGHT       2..11 = digits 1..9,0    42  '*'        43  '#'
```

Per-app exit sets differ and are **not** interchangeable — see each app below.

#### 0.3 Drawing primitive semantics

`ui.draw.rectangle((x0,y0,x1,y1), fill=...)` in Pillow fills **inclusive of both
corners**. `rectangle((0,0,240,145))` therefore paints columns 0–239 (x=240 clipped) and
rows 0–145 inclusive. The `SoftKeyBar` then repaints rows 145–174 black, which is why
the golden `eng-lcdtest.png` has its last red row at **y=144**, not 145. The C
rasteriser must use the same inclusive convention or every screen here is off by one
row.

`ui.draw.line((x0,y0,x1,y1), fill=..., width=1)` — Bresenham, endpoints inclusive.

#### 0.4 How an engineering app is reached

`AppSelector` (`ui/framework.py:307`) is the one-app-per-screen launcher. With
`ui.H = 175`:

```
header_y  = max(30, int(175 * 0.11)) = 30
icon_y    = 30 + max(24, int((145 - 30) * 0.22)) = 30 + 25 = 55
icon_cap  = min(175, max(24, 145 - 55 - 8)) = 82
```

So every engineering icon is drawn at ≤ 82 px, centred at `x = (240 - img.width)//2`,
`y = 55`, pasted **with its own alpha**. App name above it in `font_xl` at
`y = header_y - 16 = 14`, centred. The word `Select` in `font_n` at
`y = 145 + max(0, (30 - h)//2)`. Right-edge scrollbar at `x = 232`, track `y` 36 → 135,
notch a 7×7 white block. Page number (`selected_index + 1`) in `font_n`, right-aligned
at `x = 240 - 5 - w`, `y = 10`.

### 1. Engineering mode — the unlock

#### 1.1 The setting

```
key      "system.ui.engineering_mode"
default  "ON"                       (SettingsStorage.DEFAULTS, line 18)
file     /NeoDCT/User/settings.prop
```

Truthiness is decided by `_setting_is_enabled(value, default=True)`, defined **twice**
identically (`core/main.py:79`, `apps/Settings/main.py:120`):

```python
if value is None:            return default
text = str(value).strip().lower()
if text in ("1","true","on","yes","enabled"):    return True
if text in ("0","false","off","no","disabled"):  return False
return default
```

Anything unrecognised falls back to the caller's default. Both call sites pass
`default=True`. So a corrupt or nonsense value leaves engineering mode **on**.

#### 1.2 At boot (`core/main.py` lines 635–673)

```python
engineering_mode = _setting_is_enabled(get_setting("system.ui.engineering_mode","ON"), default=True)
self.engineering_mode = engineering_mode
self.apps = []
app_dirs = ["/NeoDCT/System/apps"]
if engineering_mode:
    app_dirs.append("/NeoDCT/System/engineering/apps")
for app_dir in app_dirs:
    self._scan_apps_from_dir(app_dir)
self.apps.sort(key=lambda x: x["id"])
```

`_scan_apps_from_dir` iterates `os.listdir(app_dir)` (arbitrary order — the sort is what
makes it deterministic), skips any folder with no `manifest.json`, and swallows every
per-folder error with a bare `except: pass`. If the directory does not exist it tries
`os.makedirs` and returns on failure. Each entry is:

```python
{"name": data.get("name", folder),
 "icon": f"{app_dir}/{folder}/" + data.get("icon", "icon.png"),
 "path": f"{app_dir}/{folder}",
 "exec": data.get("exec", "main.py"),
 "id":   int(data.get("id", 999))}
```

**Resulting menu order with engineering mode ON** (this is the order a golden
menu-navigation frame depends on):

```
1 Phone book · 2 Messages · 3 Call Log · 4 Settings · 6 Games · 7 Calculator ·
8 Clock · 9 Tones · 10 Koki Mobile · 11 Browser · 12 Update · 970 Music ·
971 Power · 999 Linux Shell · 9001 LCD Test · 9002 KeyMap · 9003 KeyMapI2C ·
9004 FuelGauge · 9005 ModemInfo · 9006 Downgrade · 9990 Remote Shell ·
9997 Crash · 9998 Cube Bench · 9999 Tests
```

With engineering mode OFF the list stops at 971 Power.

#### 1.3 Toggling at runtime (`apps/Settings/main.py` lines 133–173)

Settings' root menu is `VerticalList(ui, "Settings", ["Wallpaper", "Memory card",
"Engineering Mode", "About"], app_id=4)`; index 2 opens the toggle.

```python
def _show_engineering_mode(ui):
    current_enabled = _setting_is_enabled(get_setting(ENGINEERING_MODE_KEY, "ON"), default=True)
    options = ["On", "Off"]
    menu = VerticalList(ui, "Eng. Mode", options, app_id=4)   # ROOT_ID of Settings
    menu.selected_index = 0 if current_enabled else 1
    SoftKeyBar(ui).update("Select", present=False)
    selection = menu.show()
    if selection == -1:
        return                                  # BACK leaves the setting alone
    enabled = selection == 0
    set_setting(ENGINEERING_MODE_KEY, "ON" if enabled else "OFF")
    _refresh_engineering_apps(ui, enabled)
    MessageDialog(ui, f"Engineering Mode set to {'ON' if enabled else 'OFF'}.").show()
```

`_refresh_engineering_apps(ui, enabled)`:

1. returns immediately if `ui` has no `apps` attribute;
2. sets `ui.engineering_mode = bool(enabled)` inside a `try/except: pass`;
3. removes every app whose `path` contains the literal substring
   `"/NeoDCT/System/engineering/apps/"` — note the **trailing slash**;
4. if enabling, calls `ui._scan_apps_from_dir("/NeoDCT/System/engineering/apps")`;
5. re-sorts `ui.apps` by `id`, again inside `try/except: pass`.

The written value is the literal string `"ON"` or `"OFF"`. The dialog text is
`Engineering Mode set to ON.` / `... OFF.` with the trailing full stop.

**Port note.** In the C architecture Settings is a *separate process*, so it cannot
mutate the core's app list in memory. The refresh must become a message from the app
child to the core (or the core must re-scan after every app exit). Getting this wrong
is invisible until someone toggles the setting and the menu does not change — flag it
in the core/app IPC design rather than skipping it.

#### 1.4 Other consumers of the flag

* `CrashHandler._is_engineering_mode(ui, default=False)` reads `ui.engineering_mode`;
  when it is `None` the default is **False** (not True — different from the boot path).
* `apps/Update/main.py:_engineering_mode(ui)` reads `ui.engineering_mode` first and
  falls back to `str(get_setting("system.ui.engineering_mode","ON")).strip().upper()`;
  engineering mode is what lets an unsigned or badly signed package be installed after
  a second confirmation (`spec-update-system.md` owns that path).
* The build honours `NEODCT_EXCLUDE_APPS` and prunes stale directories from
  **both** `NeoDCT/System/apps` and `NeoDCT/System/engineering/apps`
  (`neodct/scripts/post-build-prune-tests.sh` lines 30–43, 77–85).

### 2. Crash (`apps/Crash/main.py`, 17 lines) — APP_ID 9997

```python
from System.ui.framework import VerticalList, SoftKeyBar
APP_ID = 9997

def run(ui):
    softkey = SoftKeyBar(ui)
    while True:
        menu = VerticalList(ui, "Crash", ["CRASH!"], app_id=APP_ID)
        softkey.update("Select", present=False)
        choice = menu.show()
        if choice == -1:
            return
        if choice == 0:
            raise RuntimeError("Intentional crash from Crash app (test)")
```

Exact points:

* `SoftKeyBar` is constructed **once**, outside the loop; the `VerticalList` is
  reconstructed each pass, so its `selected_index` resets to 0 every time.
* `softkey.update(..., present=False)` is called **before** `menu.show()`, and
  `VerticalList.show()` calls `self.draw()` first — which repaints only rows 0–145 — so
  the "Select" bar drawn beforehand survives. Order matters.
* `menu.show()` returns `-1` on key 14, `self.selected_index` on key 28, and
  `key - 2` on keys 2–10 when that index exists (so `1` on the keypad also fires it).
* The raised message string is load-bearing: it is what the crash screen's summary
  strip prints.

#### 2.1 What the crash then does (`core/CrashHandler/__init__.py`, 242 lines)

`launch_app` catches `BaseException` (but re-raises `KeyboardInterrupt` and
`IncomingCall`), prints `[OS] App crashed: Crash (/NeoDCT/System/engineering/apps/Crash/main.py)`
plus a traceback, then calls `show_app_crash(self, app_name="Crash", exc_info=...)`.

```
CRASH_IMAGE_PATH   = "/NeoDCT/System/ui/resources/CRASH.jpg"
DEFAULT_NOTICE     = "An application has crashed."
CONTINUE_KEYS      = {14, 28, 46, 50, 96}
CRASH_LOG_DIR      = "/NeoDCT/User/logs"
CRASH_LOG_PATH     = "/NeoDCT/User/logs/crash.log"
CRASH_LOG_ROTATED  = "/NeoDCT/User/logs/crash.log.1"
CRASH_LOG_MAX_BYTES = 64 * 1024
```

`log_crash(source, exc_info, note)` — never raises, returns the path or `None`:

```
============================================================        (60 '=')
time:   %Y-%m-%d %H:%M:%S (epoch <int>)
mode:   QEMU/simulation | hardware
source: <source>
uptime: <first field of /proc/uptime>s   mem: <MemAvailable or MemFree line, whitespace-collapsed>
note:   <note>                                                      (only when note given)
<full traceback, rstripped>            or  (no exception info available)
```

then `os.makedirs(CRASH_LOG_DIR, exist_ok=True)`, rotate if
`getsize(crash.log) > 65536` (`os.replace` to `crash.log.1`), append, `flush()`,
`fsync(file)`, then `fsync()` the **directory** `/NeoDCT/User/logs` *and* its parent
`/NeoDCT/User` — the comment explains why: a newly created file's directory entry is
only durable once the parent is synced. In simulation mode it also prints
`[CRASH] <source>: <summary> (report -> /NeoDCT/User/logs/crash.log)`.

`is_simulation()` is `not os.path.exists("/dev/ttyFIQ0")`, with `True` on exception.

`_exc_summary(exc_info)` → `f"{type.__name__}: {value}"`, truncated to
`text[:87] + "..."` when longer than 90 characters, `None` on any failure.

`_draw_engineering_crash_screen(ui, summary)` (engineering mode ON):

1. `ui.draw.rectangle((0, 0, 240, 175), fill="black")`
2. `Image.open("/NeoDCT/System/ui/resources/CRASH.jpg").convert("RGB")`, resized to
   240 × 175 with **LANCZOS**, pasted at (0, 0). On any failure instead draw the word
   `CRASH` centred: `x = (240 - w)//2`, `y = max(0, (145 - h)//2)`, font `font_xl` →
   `font_n` → `font_s`, first that exists.
3. If `summary` and `font_s`: `th = get_text_size(summary, font_s)[1]`, then
   `rectangle((0, 0, 240, th + 4), fill="black")` and `text((2, 2), summary,
   font=font_s, fill="white")`. Whole step wrapped in `try/except: pass`.
4. `SoftKeyBar(ui).update("Continue", present=False)` then `ui.fb.update(ui.canvas)`.

Before drawing, `_flush_input(ui)` drains `ui.keypad_fd` with 24-byte `os.read` calls
until `select` says nothing is ready. Then `_wait_for_continue` loops
`ui.wait_for_key()` until the code is in `CONTINUE_KEYS`.

Engineering mode OFF: `MessageDialog(ui, f"{message}\n{summary}").show()`, or just
`message` when there is no summary.

**In C** the whole shape changes and must be preserved by other means: there is no
exception. The core sees the child die via `waitpid()`, learns the signal, and draws the
same screen with a summary line built from the signal name and faulting address instead
of `RuntimeError: ...`. The Crash app itself becomes a deliberate `abort()` (or a null
dereference) after the same one-item menu. `CONTINUE_KEYS`, the image path, the LANCZOS
resize, the black summary strip and the log format all stay identical.
`crash-screen` is one of the 49 golden frames (sha256
`1381e6db…`), captured by calling `_draw_engineering_crash_screen(ui,
"RuntimeError: example failure")` directly.

### 3. TestsApp (`apps/TestsApp/main.py`, 31 lines) — APP_ID 9999

```python
def run(ui):
    screen_w = 240; content_bottom = 145
    ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")
    softkey = SoftKeyBar(ui)
    softkey.update("Testing123")                 # present=True -> flushes immediately
    warningmsg = MessageDialog(ui, "This is a test of the error screen")
    text = "Hello World"
    w, h = ui.get_text_size(text, ui.font_xl)    # (165, 21)
    ui.draw.text(((240 - 165)//2, (145 - 21)//2), text, font=ui.font_xl, fill="white")
    ui.fb.update(ui.canvas)                      # -> "Hello World" at (37, 62)
    while True:
        warningmsg.show()
        key = ui.wait_for_key()
        if key in (46, 28, 50):
            return
```

* `softkey.update("Testing123")` uses the default `present=True`, so it pushes a frame
  of its own before "Hello World" is drawn. Two frames leave this app before the loop.
* The same `MessageDialog` **instance** is re-shown every pass. `MessageDialog._draw`
  clears the whole 240 × 175 canvas, so "Hello World" is only ever visible for one
  frame.
* Exit needs **two** presses: one to dismiss the dialog (accept 28 or cancel 14), then
  one of 46 / 28 / 50 to leave. Key 14 dismisses but does not exit, so pressing C
  forever just re-draws the dialog.
* Golden frame `eng-tests` (sha256 `8876fdd9…`) is the dialog: warning triangle from
  `/NeoDCT/System/ui/resources/img/errorscreen/warning.png` at (8, 8), the message
  centred in `font_n` across two lines (`This is a test of` / `the error screen`), and
  `OK` on the softkey bar.

### 4. LCDTest (`apps/LCDTest/main.py`, 64 lines) — APP_ID 9001

```
KEY_NAV = 28   KEY_BACK = 14
```

Four patterns in a fixed cycle: `Red #FF0000`, `Green #00FF00`, `Blue #0000FF`,
`TV Test`. Loop body, in order: draw the pattern, `softkey.update("Next")` (default
`present=True`), `ui.fb.update(ui.canvas)` — so **two** frames are emitted per pattern.
Then `key = ui.wait_for_key()`; 28 advances `idx = (idx + 1) % 4`, 14 returns. Every
other key redraws the same pattern (the loop restarts).

Flood fill: `ui.draw.rectangle((0, 0, 240, 145), fill=colour)`.

TV pattern (`_draw_tv_pattern`), all numbers exact for W=240, content_bottom=145:

```
top_h   = int(145 * 0.7) = 101
bars    = ["#FFFFFF","#FFFF00","#00FFFF","#00FF00","#FF00FF","#FF0000","#0000FF"]   (7)
bar_w   = max(1, 240 // 7) = 34
for i, colour in enumerate(bars):
    x0 = i * 34
    x1 = 240 if i == 6 else (i + 1) * 34
    rectangle((x0, 0, x1, 101), fill=colour)
-> 0–34, 34–68, 68–102, 102–136, 136–170, 170–204, 204–240 (inclusive x1: each bar
   overpaints the previous bar's last column, and the last bar is clipped at 239)

lower_h = 145 - 101 = 44        (>0, so the lower block is drawn)
mid_y   = 101 + (44 // 2) = 123
rectangle((0, 101, 240, 123), fill="#202020")
stripes = ["#00214A","#FFFFFF","#32006A","#000000"]                                  (4)
stripe_w = max(1, 240 // 4) = 60
    x0 = i * 60;  x1 = 240 if i == 3 else (i + 1) * 60
    rectangle((x0, 123, x1, 145), fill=colour)
```

Golden frame `eng-lcdtest` (sha256 `73ffb08f…`) is the **first** pattern, plain red,
rows 0–144, with the black softkey bar and `Next` centred in `font_n`.

### 5. CubeBench (`apps/CubeBench/main.py`, 144 lines) — APP_ID 9998

```
EXIT_KEYS = {14, 28, 46, 50}
```

Setup (evaluated once, exact doubles for W=240, content_bottom=145):

```
center_x  = 240 // 2  = 120
center_y  = 145 // 2  =  72
size      = min(240, 145) * 0.22 = 31.9
fov       = min(240, 145) * 1.1  = 159.5
view_dist = size * 5.5           = 175.45
```

Vertices, in this order (the edge table indexes into it):

```
0 (-s,-s,-s)  1 ( s,-s,-s)  2 ( s, s,-s)  3 (-s, s,-s)
4 (-s,-s, s)  5 ( s,-s, s)  6 ( s, s, s)  7 (-s, s, s)
```

Edges, in this order (draw order is visible where lines overlap):

```
(0,1)(1,2)(2,3)(3,0)  (4,5)(5,6)(6,7)(7,4)  (0,4)(1,5)(2,6)(3,7)
```

Rotations — applied X, then Y, then Z, each on the result of the previous:

```
rot_x(v,a): (x,  y*cos a - z*sin a,  y*sin a + z*cos a)
rot_y(v,a): (x*cos a + z*sin a,  y,  -x*sin a + z*cos a)
rot_z(v,a): (x*cos a - y*sin a,  x*sin a + y*cos a,  z)
```

Projection:

```
denom = z + view_dist;   if denom < 0.1: denom = 0.1
scale = fov / denom
sx = int(center_x + x*scale)      # C: (int) truncation toward zero, same as Python int()
sy = int(center_y + y*scale)
```

Frame loop, in exact order:

1. `now = time.perf_counter()`; `dt = now - last_time`; `last_time = now`;
   `if dt <= 0.0: dt = 0.001`
2. `key = ui.read_keypress(0)` — **non-blocking**, timeout 0. `if key in EXIT_KEYS: return`
3. `ax += 1.30*dt`, `ay += 1.10*dt`, `az += 0.85*dt`
4. `ui.draw.rectangle((0, 0, 240, 145), fill="black")`
5. project all 8 vertices, then `ui.draw.line((x1,y1,x2,y2), fill="white", width=1)` for
   each of the 12 edges
6. `fps_inst = 1.0/dt` (computed, never displayed); `frame_counter += 1`;
   `elapsed = now - fps_window_start`; if `elapsed >= 0.5` then
   `fps_display = frame_counter/elapsed`, `frame_counter = 0`, `fps_window_start = now`
7. `ui.draw.text((6, 4), "3D Cube", font=font_s, fill="white")`
8. `fps_text = "FPS %.1f" % fps_display`; `fw = get_text_size(fps_text, font_s)[0]`;
   `text((240 - fw - 6, 16), fps_text, font=font_s, fill="white")`
9. `hint = "BACK/OK to exit"`; `hw, hh = (137, 13)`;
   `text(((240 - 137)//2, 145 - 13 - 4), hint, font=font_s, fill="gray")` → **(51, 128)**
10. `softkey.update("Exit", present=False)`
11. `ui.fb.update(ui.canvas)`

Initial state: `ax = ay = az = 0.0`, `fps_display = 0.0`, `frame_counter = 0`,
`last_time = fps_window_start = time.perf_counter()` taken **before** the loop.

`"gray"` in Pillow is `(128, 128, 128)`; the C palette must use the same value for
every `fill="gray"` in this document.

#### 5.1 Reproducing the golden frame

`eng-cubebench` (sha256 `a4729594…`) is captured by
`shoot_docs.shoot_engineering_apps` → `run_app(ui, "Cube Bench", keys=[])` with the
default `frame_budget=240`, saving frame index `-1`. Under `goldenframe.py` the clock is
virtual:

```
EPOCH = 1704112496.0      TICK = 0.1      SEED = 20240101
time.perf_counter() == time.monotonic() == EPOCH + frame * TICK
frame advances by 1 on every fb.update()
```

Consequences the C `nd-shoot` harness must reproduce bit-for-bit:

* the first iteration sees `dt == 0.0` and substitutes `0.001`;
* every later iteration sees `dt = (EPOCH + k*0.1) - (EPOCH + (k-1)*0.1)`, which because
  of the large epoch is **`0.09999990463256836`**, not `0.1`;
* after 240 iterations the accumulated angles are, in IEEE-754 double:

```
ax = 31.071300123977615
ay = 26.291100104904135
az = 20.315850081062308
fps_display = 10.0   ->  "FPS 10.0",  fw = 68,  drawn at (166, 16)
```

Implement the virtual clock the same way (`epoch + frame*tick` in `double`, subtracting
two such values) or the cube lands at a different angle and the frame will not match.

### 6. FuelGauge (`apps/FuelGauge/main.py`, 104 lines) — APP_ID 9004

```
KEY_NAV = 28   KEY_BACK = 14   REFRESH_S = 1.0
HW_REQUIRED_MSG = ("No MAX1704x fuel gauge found, so BatteryService is running its QEMU "
                   "simulation stub. This app needs real hardware.")
```

Entry gate:

```python
battery = getattr(ui, "battery", None)
if battery is None or not battery.hardware:
    MessageDialog(ui, HW_REQUIRED_MSG).show()
    return
```

Main loop:

```python
flash = ""; flash_until = 0.0; last_draw = 0.0
while True:
    now = time.monotonic()
    if flash and now >= flash_until:
        flash = ""; last_draw = 0.0                 # force an immediate repaint
    if now - last_draw >= 1.0:
        _draw_readout(ui, battery, flash)
        softkey.update("QStart", present=False)
        ui.fb.update(ui.canvas)
        last_draw = now
    key = ui.read_keypress(0.1)
    if key == 14: return
    if key == 28:
        ok = battery.quickstart()
        flash = "quick-start sent" if ok else "quick-start FAILED"
        flash_until = time.monotonic() + 2.0
        last_draw = 0.0
```

`last_draw = 0.0` initially, so the first pass always draws (`now - 0.0 >= 1.0`).

`_draw_readout`:

```
rectangle((0, 0, 240, 145), fill="black")
text((5, 0), "FuelGauge", font=font_xl, fill="white")
line((0, 30, 240, 30), fill="white")
snap = battery.debug_snapshot()
rows = [("MODE","SIMULATION")] if snap is None else _rows_from_snapshot(snap)
y = 36
line_h = max(15, (145 - 36 - 16) // max(1, len(rows)))        # (93 // n), floor 15
for label, value in rows:
    text((8,  y), label, font=font_s, fill="gray")
    text((70, y), value, font=font_s, fill="white")
    y += line_h
if snap is not None:
    bus_text = "i2c-%d @ 0x%02X" % (snap["bus"], snap["addr"])
    bw = get_text_size(bus_text, font_s)[0]
    text((240 - 5 - bw, 145 - 14), bus_text, font=font_s, fill="gray")
if flash:
    text((8, 145 - 14), flash, font=font_s, fill="gray")
```

`line_h` values that occur: 6 rows → 15, 1 row → 93.
Bottom row `y = 131`; `i2c-3 @ 0x36` is 109 px wide → drawn at **x = 126**.

`_rows_from_snapshot(snap)`:

```python
if snap.get("error"):
    return [("ERROR", snap["error"][:24])]         # truncated to 24 characters
crate = snap["crate"];  smoothed = snap["smoothed_v"]
return [
  ("VCELL", "%.4f V  (0x%04X)" % (snap["vcell"], snap["raw_vcell"])),
  ("SOC",   "%.2f %%  (0x%04X)" % (snap["soc"],  snap["raw_soc"])),
  ("CRATE", "n/a (17043/44)" if crate is None else "%+.2f %%/hr" % crate),
  ("GAUGE", "%d/4  avg %s" % (snap["level"], "--" if smoothed is None else "%.3f V" % smoothed)),
  ("VER",   "0x%04X" % snap["version"]),
  ("CFG",   "0x%04X" % snap["config"]),
]
```

Note the **double spaces** inside `"%.4f V  (0x%04X)"`, `"%.2f %%  (0x%04X)"` and
`"%d/4  avg %s"`. Values are drawn at x = 70 and are **not** clipped by the app —
anything wider than 170 px runs off the right edge and is cut by the canvas.

#### 6.1 What `debug_snapshot()` returns (`core/BatteryService/__init__.py:251`)

Returns `None` when `not self.hardware`. Otherwise a dict seeded with
`bus`, `addr`, `version`, `level` (0–4), `smoothed_v`, then a fresh read of four
registers; on any exception it adds `"error": str(exc)` and returns early. On success:

```
raw_vcell, vcell = raw_vcell * VCELL_LSB
raw_soc,   soc   = raw_soc / 256.0
raw_crate, crate = None if raw_crate == 0xFFFF else signed16(raw_crate) * CRATE_LSB
config
```

`quickstart()` writes `QUICKSTART_MODE` to `REG_MODE` and returns `True`, or prints
`[BATT] Quick-start failed: <exc>` and returns `False`; returns `False` immediately when
not hardware.

#### 6.2 Reproducing the golden frame

`eng-fuelgauge` (sha256 `4e7286e2…`) is captured with `simulate_status()` forcing
`ui.battery.hardware = True` while `ui.battery.fd` is still `None`. So the gate passes,
`debug_snapshot()` reaches `os.write(None, ...)`, and the error path fires with

```
str(exc)      = "'NoneType' object cannot be interpreted as an integer"
str(exc)[:24] = "'NoneType' object cannot"
```

giving a single row `("ERROR", "'NoneType' object cannot")` at y = 36 (line_h 93), the
value 217 px wide at x = 70 so it is clipped at the canvas edge, plus
`i2c-3 @ 0x36` at (126, 131) and `QStart` on the softkey bar. The C harness must
reproduce that same stub state to match the frame.

### 7. Modem (`apps/Modem/main.py`, 296 lines) — APP_ID 9005

```
KEY_NAV = 28   KEY_BACK = 14   REFRESH_S = 1.0
PAGES = ("RADIO", "SIM", "DATA")
BOOT_STATUS_FILE     = "/tmp/modem.status"
MODEM_DEFAULTS_FILE  = "/etc/default/modem"
DEFAULT_APN          = "fast.t-mobile.com"
REG_NAMES = {None:"--", 0:"NOT REG", 1:"HOME", 2:"SEARCHING", 3:"DENIED",
             4:"UNKNOWN", 5:"ROAMING"}
```

Entry gate: `modem = getattr(ui, "modem", None)`; if `None`,
`MessageDialog(ui, "ModemService is not running.").show()` and return. **It does not
bail out in simulation mode** — that is deliberate and documented in the module header.

Loop:

```python
page = 0; sim_rows_cache = None; last_draw = 0.0
while True:
    now = time.monotonic()
    if now - last_draw >= 1.0:
        rows = _radio_rows(modem)          if page == 0 else \
               (sim_rows_cache := sim_rows_cache or _sim_rows(modem)) if page == 1 else \
               _data_rows()
        _draw_page(ui, modem, page, rows)
        softkey.update("Next" if page < 2 else "Exit", present=False)
        ui.fb.update(ui.canvas)
        last_draw = now
    key = ui.read_keypress(0.1)
    if key == 14: return
    if key == 28:
        page += 1
        if page >= 3: return               # Exit from page 3 leaves the app
        last_draw = 0.0                    # repaint immediately on page change
```

(the walrus above is shorthand for the original `if sim_rows_cache is None:
sim_rows_cache = _sim_rows(modem)` — the SIM page is queried **once per app run** and
cached, because identity does not change mid-session.)

`_draw_page`:

```
rectangle((0, 0, 240, 145), fill="black")
text((5, 0), "Modem", font=font_xl, fill="white")
page_name = PAGES[page];  pw = get_text_size(page_name, font_s)[0]
text((240 - 5 - pw, 8), page_name, font=font_s, fill="gray")      # RADIO -> x=186
line((0, 30, 240, 30), fill="white")
y = 36;  line_h = max(15, (145 - 36 - 16) // max(1, len(rows)))
for label, value in rows:
    text((8,  y), label,      font=font_s, fill="gray")
    text((70, y), str(value), font=font_s, fill="white")
    y += line_h
mode = modem.port if modem.hardware else "SIMULATION"
text((8, 131), mode, font=font_s, fill="gray")
pos = "%d/%d" % (page + 1, 3);  posw = get_text_size(pos, font_s)[0]
text((240 - 5 - posw, 131), pos, font=font_s, fill="gray")        # "1/3" -> x=210
```

`line_h`: 6 rows → 15, 5 rows → 18.

#### 7.1 RADIO page (`_radio_rows`)

From `modem.status_snapshot()` (`core/ModemService/__init__.py:1058`), which returns
`hardware, port, imei, state, csq, bars, reg_stat, registered, operator, caller_id`.

```
csq_text  = "--"                    if csq is None
          = "99 (no signal)"        if csq == 99
          = "%d/31  %d dBm" % (csq, -113 + 2*csq)      otherwise   (double space)
reg_text  = REG_NAMES.get(stat, str(stat))
            + ("  (CEREG %s)" % stat if stat is not None else "")   (double space)
rows = [("OPER", snap["operator"] or "--"),
        ("REG",  reg_text),
        ("CSQ",  csq_text),
        ("BARS", "--" if snap["bars"] is None else "%d/4" % snap["bars"]),
        ("CALL", snap["state"])]
if not snap["hardware"]:
    ttys = sorted(n for n in os.listdir("/dev") if n.startswith("ttyUSB"))
    rows.append(("PORTS", ",".join(ttys) if ttys else "no ttyUSB nodes!"))
```

Golden frame `eng-modem` (sha256 `f11f60c5…`) is this page in simulation:
`OPER --`, `REG --`, `CSQ --`, `BARS 4/4` (the stub overrides `signal_level`),
`CALL IDLE`, `PORTS no ttyUSB nodes!`, bottom-left `SIMULATION`, bottom-right `1/3`,
softkey `Next`, page tag `RADIO` at (186, 8), 6 rows so line_h = 15.

#### 7.2 SIM page (`_sim_rows`) — one-shot AT queries

When `not modem.hardware`, all six values are the literal `"n/a (sim)"`.

Otherwise, in this exact order, each through `modem.send_at(cmd, timeout=3.0)` which
returns `(final, lines)`:

```
AT+CPIN?   final=="OK"  -> sim = _first_content(lines, "+CPIN:") or "?"
           final is None-> sim = "no reply"
           else         -> sim = "NOT DETECTED"

AT+CNUM    number defaults to "(not on SIM)"; if final=="OK", walk lines and for the
           first line starting "+CNUM:" with at least 4 '"' characters take
           line.split('"')[3]; use it if non-empty; break after the first "+CNUM:" line
           regardless.

AT+CICCID  iccid = _first_content(lines, "+ICCID:") if final=="OK" else None
           if falsy: AT+CCID -> _first_content(lines, "+CCID:") if final=="OK" else None

AT+CIMI    imsi = _first_content(lines, "+CIMI:") if final=="OK" else None
AT+CGMR    fw   = _first_content(lines, "+CGMR:") if final=="OK" else None

rows = [("SIM", sim), ("NUM", number), ("IMEI", modem.imei or "--"),
        ("ICCID", iccid or "--"), ("IMSI", imsi or "--"), ("FW", _shorten(fw or "--"))]
```

`_first_content(lines, prefix)`: skip blank/whitespace-only lines; for the first
non-blank line, if `line.upper().startswith(prefix.upper())` return
`line.split(":", 1)[1].strip()`, else return the line itself. `None` if nothing.

`_shorten(text, limit=24)`: `str(text)`; if `len <= 24` return unchanged; else
`keep = (24 - 2) // 2 = 11` and return `text[:11] + ".." + text[-11:]`.

#### 7.3 DATA page (`_data_rows`)

```
("BOOT", contents of /tmp/modem.status, stripped, or "(no S45modem run)")
("IF",   "<iface> UP" / "<iface> DOWN", or "none found")
("IPV6", _shorten(first global IPv6 on that iface) or "--")
("APN",  _shorten(configured APN))
("DNS",  _shorten(dns row))
```

`_wwan_interface()` — first pass: for every name in `sorted(os.listdir("/sys/class/net"))`,
resolve `/sys/class/net/<name>/device/driver` with `os.path.realpath` and return the
name when the basename is exactly `qmi_wwan`. Second pass: for each prefix in
`("ww", "rmnet", "usb")` in that order, return the first `sorted` name that starts with
it. Otherwise `None`. The comment explains why the driver wins over the name: eudev
renames `wwan0` to `wwp<path>`.

`_iface_up(name)` — read `/sys/class/net/<name>/flags`, `int(flags, 16) & 1`;
`False` on `TypeError`/`ValueError`.

`_global_ipv6(ifname)` — read `/proc/net/if_inet6` line by line; a line qualifies when
it has ≥ 6 whitespace fields, `fields[5] == ifname` and `fields[3] == "00"` (global
scope); return `socket.inet_ntop(AF_INET6, bytes.fromhex(fields[0]))` for the first
match. Any exception → `None`.

`_configured_apn()` — read `/etc/default/modem`; for each stripped line starting
`MODEM_APN=`, return `line.split("=", 1)[1].strip().strip('"')` or `DEFAULT_APN` when
that is empty. Falls through to `DEFAULT_APN` = `"fast.t-mobile.com"`.

`_dns_row()` — read `/etc/resolv.conf`; collect `ln.split()[1]` for every line starting
`nameserver` with more than one field; return the **first entry containing a colon**
(the DNS64/IPv6 one) in preference, else the first entry, else `"--"`.

`_read_file(path)` returns the stripped contents or `None` on any exception;
`_listdir(path)` returns `[]` on any exception.

### 8. KeypadMapper (`apps/KeypadMapper/main.py`, 303 lines) — APP_ID 9002, menu name `KeyMap`

```
ROOT_ID = 10                      # the "10" in the "10-1/16" progress tag
OUTPUT_PATH = "/NeoDCT/User/keymap.json"
KEY_MENU = 50
GPIO_REQUIRED_MSG    = "This app requires GPIO. GPIO devices not found. This application can not run in QEMU."
GPIOZERO_REQUIRED_MSG = "gpiozero is missing. Install python3-gpiozero to run keypad mapping."
KEYPAD_ROWS_ENV = "NEODCT_KEYPAD_ROWS"    KEYPAD_COLS_ENV = "NEODCT_KEYPAD_COLS"
DEFAULT_ROW_PINS = [21, 20, 16, 12]       DEFAULT_COL_PINS = [26, 19, 13, 6]
```

**Read this before porting the scanner.** `gpiozero` is *not* in either defconfig
(`neodct/configs/neodct_qemu_defconfig` and `luckfox_pico_mini_defconfig` enable only
`PYTHON3`, `SSL`, `SQLITE`, `XZ`, `PILLOW`, `MUTAGEN`, `MINIAUDIO`). So on every shipped
image the module-level `from gpiozero import Button, OutputDevice` fails and
`GPIOZERO_IMPORT_ERROR` is a string. The **only reachable behaviour on a real phone** is:

```python
def run(ui):
    if not _gpio_available():                      # len(glob("/dev/gpiochip*")) == 0
        MessageDialog(ui, GPIO_REQUIRED_MSG).show(); return
    if GPIOZERO_IMPORT_ERROR is not None:
        print(f"[KEYMAP] gpiozero import failed: {GPIOZERO_IMPORT_ERROR}")
        MessageDialog(ui, GPIOZERO_REQUIRED_MSG).show(); return
    KeypadMapper(ui).run()
```

— i.e. `GPIO_REQUIRED_MSG` under QEMU (no gpiochips) and `GPIOZERO_REQUIRED_MSG` on the
Luckfox (gpiochips exist, gpiozero does not). A strict 1:1 port reproduces exactly that
and stops. Porting the scanner for real (via `/dev/gpiochip*` `GPIO_V2_LINE` ioctls)
would be a **behaviour change** — see Risks. The rest of this section documents the
algorithm either way, because the I²C sibling shares it.

`_parse_pins(raw, fallback)`: strip; empty → `list(fallback)`; else split on `,`, strip
each, skip empties, `int()` each; empty result → `list(fallback)`.
`_matrix_pins()` wraps both parses in one `try`; on any exception prints
`[KEYMAP] Invalid pin override: {exc}; using defaults.` and uses both defaults.

`MatrixScanner` (gpiozero flavour):

* `rows = [OutputDevice(pin, initial_value=True) for pin in row_pins]`
* `cols = [Button(pin, pull_up=True) for pin in col_pins]`
* `scan_once()`: for each row in order — `row.off()`, `time.sleep(0.001)`, then for each
  col in order, if `col.is_pressed` record `(row_idx, col_idx)` and **break**;
  `row.on()`; break out of the row loop if something was found. If nothing found set
  `self._held = None` and return `None`. If the found position equals `self._held`
  return `None` (edge detect). Otherwise store and return it.
  This stops at the **first** hit, so there is no rollover — unlike the I²C scanner.
* `close()`: for every row `row.on()` then `row.close()`, each in its own
  `try/except: pass`; then `col.close()` for every col, same guarding.

`KEY_TARGETS` — 16 entries, captured in this order:

```
("navikey","NaviKey") ("clear","C") ("up","Up") ("down","Down")
("num_1","1") ("num_2","2") ("num_3","3") ("num_4","4") ("num_5","5")
("num_6","6") ("num_7","7") ("num_8","8") ("num_9","9") ("num_0","0")
("star","*") ("hash","#")
```

`_draw_capture_prompt(label, index, total)` — index is **1-based**:

```
rectangle((0, 0, 240, 175), fill="black")        # note: full height, softkey area too
title = "Keypad Mapper";  tw = 193 (font_n)
text(((240 - tw)//2, 8), title, font=font_n, fill="white")          -> (23, 8)
line((12, 32, 228, 32), fill="white")
progress = f"10-{index}/{total}";  pw = get_text_size(progress, font_s)[0]
text((240 - pw - 8, 38), progress, font=font_s, fill="gray")
body = [f"Press: {label}", "", "Capture one keypad button now.", "Menu key cancels.",
        f"Rows: {self.row_pins}", f"Cols: {self.col_pins}"]
y = 56;  line_h = get_text_size("Ag", font_s)[1] + 4 = 19
for raw in body:
    lines = [""] if raw == "" else _wrap_text(ui, raw, 240 - 16, font_s)
    for line in lines:
        if y > 145 - 19: break                    # y > 126
        text((8, y), line, font=font_s, fill="white")
        y += line_h
softkey.update("Capture", present=False);  ui.fb.update(ui.canvas)
```

`f"Rows: {self.row_pins}"` interpolates a **Python list repr** —
`Rows: [21, 20, 16, 12]`, square brackets, comma-space separators. Reproduce that
formatting literally.

`_wrap_text(ui, text, max_width, font)` — greedy word wrap on `str.split()` (any
whitespace, no empty tokens); a candidate is `f"{current} {word}".strip()` when
`current` is non-empty else `word`; it fits when `get_text_size(candidate, font)[0]
<= max_width`. Words longer than `max_width` are **not** broken — they go on a line of
their own and overflow. Empty input returns `[""]`.

**The clipping quirk, which is visible and must be preserved.** With `max_width = 224`
the measured widths are:

```
"Press: NaviKey"                    130   fits
"Capture one keypad button now."    280   wraps to "Capture one keypad" / "button now."
"Menu key cancels."                 156   fits
"Rows: [21, 20, 16, 12]"            173   fits
"Cols: [26, 19, 13, 6]"             158   fits
```

but `y` runs 56, 75, 94, 113 and the next line would be at 132 > 126, so the loop
breaks. The rendered prompt is only ever **four lines**:

```
y=56   Press: NaviKey
y=75   (blank)
y=94   Capture one keypad
y=113  button now.
```

"Menu key cancels." and both pin lines are computed and never drawn (the `break` exits
only the inner loop, and the outer loop's next entry breaks immediately too).

`_wait_for_matrix_press()`:

```python
while True:
    key = self.ui.read_keypress(0.01)
    if key == 50: return None                 # MENU cancels
    pos = self.scanner.scan_once()
    if pos is not None: return pos
    time.sleep(0.01)
```

`_capture_keymap()` — for each of the 16 targets, 1-based index:

* redraw the prompt, wait for a press;
* `None` → `MessageDialog(ui, "Calibration canceled. Keymap not saved.").show()` and
  return `None`;
* an already-used `(row, col)` →
  `MessageDialog(ui, f"Matrix R{row_idx} C{col_idx} is already mapped. Press a different key for {label}.").show()`
  and retry the **same** target;
* otherwise record and move on:

```python
keymap_by_name[name] = {"label": label, "row": int(row_idx), "col": int(col_idx),
                        "row_pin": int(self.row_pins[row_idx]),
                        "col_pin": int(self.col_pins[col_idx])}
```

`_save_keymap()` — `os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)` then
`json.dump(payload, f, indent=2, sort_keys=True)` followed by a single `"\n"`, UTF-8:

```json
{
  "format": "neodct.keymap.v2.matrix",
  "generated_at_unix": <int(time.time())>,
  "output": "/NeoDCT/User/keymap.json",
  "driver": "gpiozero-matrix",
  "row_pins": [...], "col_pins": [...],
  "keys": { "<name>": {"label":..,"row":..,"col":..,"row_pin":..,"col_pin":..}, ... },
  "by_matrix": { "<row>,<col>": "<name>", ... },
  "by_code": {}
}
```

`indent=2` + `sort_keys=True` means the C writer must emit keys in **ASCII sort order**
at every level, two-space indent, `": "` after each key, and no trailing comma — this is
a file other tools read and a human diffs.

`run()` (the class method), in order:

1. `MessageDialog(ui, "This tool captures GPIO keypad matrix presses and writes JSON to /NeoDCT/User/keymap.json.", title="Keypad Mapper").show()`
2. `try: _open_scanner(); captured = _capture_keymap(); if not captured: return;
   _save_keymap(captured)`
3. `except Exception as exc: print("[KEYMAP] Capture/save error:", exc);
   MessageDialog(ui, f"Failed to write keymap: {exc}").show(); return`
4. `finally: _close_scanner()`
5. `MessageDialog(ui, f"Keymap saved to\n{OUTPUT_PATH}").show()`

`_open_scanner()` also prints
`[KEYMAP] GPIO matrix scanner ready. rows={rows} cols={cols}`.

### 9. KeypadMapperI2C (`apps/KeypadMapperI2C/main.py`, 273 lines) — APP_ID 9003, menu name `KeyMapI2C`

Same structure as §8. Differences, all of them:

```
I2C_REQUIRED_MSG = "This app requires I2C. No /dev/i2c-* devices found. This application can not run in QEMU."
I2C_BUS_ENV  = "NEODCT_I2C_KEYPAD_BUS"      I2C_ADDR_ENV = "NEODCT_I2C_KEYPAD_ADDR"
defaults come from System/hw/pcf8575_keypad:
    DEFAULT_BUS = 3   DEFAULT_ADDR = 0x20
    DEFAULT_ROW_PINS = [0, 1, 2, 3]   DEFAULT_COL_PINS = [4, 5, 6, 7]
```

* Gate: `run(ui)` shows `I2C_REQUIRED_MSG` when `glob("/dev/i2c-*")` is empty, else runs.
  There is no second gate — the driver is in-tree, not a missing pip package, so **this
  app actually works on the device**.
* `_parse_addr(raw, fallback)`: strip; empty → fallback; `int(text, 16)` when it starts
  `0x`/`0X` (case-insensitive), else `int(text)`.
* `_i2c_config()` wraps rows/cols/bus/addr parsing in one `try`; on exception prints
  `[KEYMAP-I2C] Invalid override: {exc}; using defaults.` and uses all four defaults.
  Bus is `int(os.environ.get(I2C_BUS_ENV, "").strip() or DEFAULT_BUS)`.
* Title is `"Keypad Mapper I2C"` (238 px in `font_n`, centred at x = 1).
* Body lines 5 and 6 differ:

```
f"Bus: /dev/i2c-{self.i2c_bus} Addr: 0x{self.i2c_addr:02X}"     -> "Bus: /dev/i2c-3 Addr: 0x20"   (228 px)
f"Rows: P{self.row_pins} Cols: P{self.col_pins}"                -> "Rows: P[0, 1, 2, 3] Cols: P[4, 5, 6, 7]"  (298 px)
```

  The `P` prefixes the whole list, not each pin. Both wrap, and both are past the
  y > 126 cut-off, so — exactly as in §8 — only four lines are ever drawn.
* Intro dialog: `"This tool captures PCF8575 I2C keypad presses and writes JSON to
  /NeoDCT/User/keymap.json."`, `title="Keypad Mapper I2C"`.
* **It suspends the UI's own keypad backend** so two scanners cannot drive the same
  expander:

```python
suspended_input = getattr(self.ui, "matrix_input", None)
self.ui.matrix_input = None
try:  ...
finally:
    self._close_scanner()
    self.ui.matrix_input = suspended_input
```

  Consequence: on keypad-only hardware `ui.read_keypress()` now falls through to evdev,
  which does not exist, so the MENU-key cancel is unreachable and the operator must
  finish all 16 captures or power-cycle. Preserve or fix deliberately, not by accident.
* `_open_scanner()` prints
  `[KEYMAP-I2C] Scanner ready. bus={bus} addr=0x{addr:02X} rows={rows} cols={cols}`.
* Error print prefix is `[KEYMAP-I2C]`.
* Payload differs in three fields:

```json
"format": "neodct.keymap.v3.matrix.i2c",
"driver": "pcf8575-i2c",
"i2c_bus": <int>, "i2c_addr": <int>
```

  (`i2c_addr` is written as a **decimal integer**, e.g. `32` for 0x20 — that is what
  `core/main.py:_load_matrix_keymap` reads back.)

#### 9.1 `I2CMatrixScanner`, the scanner it drives (`System/hw/pcf8575_keypad.py`)

Owned by `spec-hw-input.md`, repeated here because the mapper's timing depends on it:

```
I2C_SLAVE ioctl = 0x0703
constructor: validate every pin is 0..15 and unique (ValueError otherwise),
             open the chip, write16(0xFFFF)
_raw_scan(): for each row pin in order:
                 write16(0xFFFF & ~(1 << row_pin))
                 sleep 0.0005
                 value = read16()
                 for each col pin: pressed when ((value >> col_pin) & 1) == 0
             write16(0xFFFF) at the end; returns the SET of all pressed (row,col)
scan_once(): edge-detect with per-key release debounce (RELEASE_SCANS consecutive
             misses before a key is considered released); new presses are sorted and
             the first is returned, the rest queued in _pending and returned one per
             later call; returns None when nothing is new.
```

### 10. LinuxShell (`apps/LinuxShell/main.py`, 120 lines) — APP_ID 999, menu name `Linux Shell`

This is the one that needs the most thought in C, so the mechanism is spelled out.

**It is not a terminal emulator.** The app draws nothing. It hands the physical screen
to the kernel's own framebuffer console (`fbcon`) by switching virtual terminals, runs
`/bin/sh -i` attached to that terminal, waits for it to exit, and switches back. All
rendering — the font, the scrollback, the cursor, the escape-sequence handling — is
`fbcon` inside the kernel.

```python
shell_vt = int(os.environ.get("NEODCT_SHELL_VT", "2"))     # default VT 2
ui_vt    = int(os.environ.get("NEODCT_UI_VT",    "1"))     # default VT 1
tty_shell = f"/dev/tty{shell_vt}"                          # "/dev/tty2"

chvt = shutil.which("chvt")
if not chvt: return                    # silent no-op; keeping the UI alive beats ioctls

env = os.environ.copy()
env["PS1"]  = "NeoDCT # "
env["TERM"] = "linux"

if not _run_quiet([chvt, str(shell_vt)], timeout=1.0): return

_write_tty(tty_shell, b"\x1b[?25h")                        # show cursor
_write_tty(tty_shell, b"Type exit to go back to the NeoDCT UI\r\n\r\n")

bridge = _start_t9_bridge(ui)
if bridge is not None:
    _write_tty(tty_shell, b"T9 keypad active: 2-9 letters, 0 space, "
                          b"1 symbols, # mode, C backspace\r\n\r\n")

try:
    with open(tty_shell, "r+b", buffering=0) as t:
        p = subprocess.Popen(["/bin/sh", "-i"], stdin=t, stdout=t, stderr=t,
                             env=env, close_fds=True)
        p.wait()
except Exception:
    pass
finally:
    if bridge is not None:
        try: bridge.stop()
        except Exception: pass
    _write_tty(tty_shell, b"\x1b[?25l")                    # hide cursor again
    _run_quiet([chvt, str(ui_vt)], timeout=1.0)
    time.sleep(0.05)                                       # settle before the UI redraws
return
```

Helpers:

* `_which(name)` = `shutil.which(name)` — a `PATH` lookup. `PATH` on the device is
  extended by `/etc/profile.d/neodct-path.sh` with `/NeoDCT/System/hw`, but `chvt` comes
  from busybox in `/usr/bin` or `/bin`.
* `_run_quiet(args, env=None, timeout=None)` — `subprocess.run` with `check=False`,
  stdout and stderr to `DEVNULL`; returns `True` unless an exception escaped (so a
  non-zero exit still counts as success; only a spawn failure or timeout returns
  `False`).
* `_write_tty(path, data)` — `open(path, "wb", buffering=0)`, write, flush, swallow
  every exception.
* `_start_t9_bridge(ui)` — `from System.hw.t9_uinput import start_shell_bridge; return
  start_shell_bridge(ui)`, wrapped in a bare `try/except: return None`. So a missing
  module, a missing `/dev/uinput`, or a permissions error all degrade to "no T9", never
  to a crash.

Header comment states the deliberate constraints: *"No VT ioctls, no KDSETMODE, no
openvt (avoids common hangs)"* and the requirements *fbcon enabled, `/dev/ttyN` exists,
`chvt` available*. The kernel command line carries `vt.global_cursor_default=0`
(`neodct/tools/run_qemu.sh:102`), which is why the app turns the cursor on explicitly
and off again on the way out.

#### 10.1 The T9 bridge (`System/hw/t9_uinput.py`, 354 lines)

Only reachable when `ui.matrix_input is not None` — i.e. keypad-only hardware. On QEMU
`start_shell_bridge` returns `None` immediately because a real keyboard already reaches
the console.

```
start_shell_bridge(ui, keyboard_factory=None):
    matrix = getattr(ui, "matrix_input", None); if None -> return None
    keyboard = (keyboard_factory or UInputKeyboard)()
        on exception: print(f"[T9] uinput keyboard unavailable: {exc}"); return None
    bridge = T9ShellBridge(matrix.read_key, keyboard); bridge.start(); return bridge
```

`UInputKeyboard` opens `/dev/uinput` `O_WRONLY|O_NONBLOCK`, then:

```
UI_SET_EVBIT  = 0x40045564   with EV_KEY (1)
UI_SET_KEYBIT = 0x40045565   once per keycode in _needed_keycodes()
UI_DEV_CREATE = 0x5501       UI_DEV_DESTROY = 0x5502      BUS_VIRTUAL = 0x06
write struct uinput_user_dev: name[80]="neodct-t9-keypad", bustype=0x06,
    vendor=0x1, product=0x1, version=1, ff_effects_max=0, then 256 zero int32
    (Python: struct "80s4HI64i64i64i64i")
sleep 0.2                    # let the kernel/console bind the new device
```

`_needed_keycodes()` = every value in `_PLAIN` ∪ `_SHIFTED` ∪ `{KEY_LEFTSHIFT=42}` ∪
`PASSTHROUGH_CODES`, sorted.

`_emit` writes a `struct input_event` with **native long** timeval (`"llHHi"`: 16 bytes
on 32-bit ARM, 24 on the 64-bit test host), `sec = int(time.time())`,
`usec = int((now - sec) * 1e6)`. `send_key(code, shift)` emits, each followed by its own
`EV_SYN/SYN_REPORT 0`: optional shift-down, key-down, key-up, optional shift-up.

`PASSTHROUGH_CODES = (28, 14, 103, 105, 106, 108)` — enter, clear, up, left, right,
down. NeoDCT keypad codes *are* Linux keycodes, so no translation table is needed.

`T9ShellBridge.handle_code(code)`:

```
if code in PASSTHROUGH_CODES:  engine.reset(); keyboard.send_key(code); return
op = engine.press(code)                      # System.hw.t9_engine.T9Engine
None      -> nothing
("append", ch)  -> keyboard.type_char(ch)
("replace", ch) -> keyboard.backspace(); keyboard.type_char(ch)
("mode", label) -> nothing (the shell prompt has no mode indicator)
```

`start()` spawns a **daemon thread** named `t9-shell-bridge` running
`self._read_key(0.05)` in a loop; a read exception sleeps 0.1 s and continues, a
`handle_code` exception is swallowed. `stop()` sets the event, `join(timeout=2.0)`, then
`keyboard.close()` (which does `UI_DEV_DESTROY` then `close`, each guarded).

The on-screen hint text names the mapping: `2-9 letters, 0 space, 1 symbols, # mode,
C backspace`. The multi-tap tables live in `System/hw/t9_engine.py`
(`LETTER_CYCLES` `2:abc 3:def 4:ghi 5:jkl 6:mno 7:pqrs 8:tuv 9:wxyz`, key-1 punctuation
cycle `".,?!'\"1-()@/:_;+#*=<>"`, `#` cycles word → abc → ABC → 123, 1.0 s multi-tap
timeout) — owned by `spec-hw-input.md`.

`_PLAIN` / `_SHIFTED` are the US-layout char → keycode tables; reproduce them verbatim
when porting `nd_uinput.c`.

#### 10.2 What LinuxShell needs in C

Nothing about the design changes. The C app is a short program that:

1. resolves `chvt` on `PATH` (`access(X_OK)` over the `PATH` entries, or hard-code
   `/usr/bin/chvt` with a fallback — but keep the "not found → return silently"
   behaviour);
2. `posix_spawn`/`fork`+`execve` of `chvt <n>` with a 1 s wait, stdout/stderr to
   `/dev/null`;
3. `open("/dev/tty2", O_WRONLY)` unbuffered writes of the three literal byte strings;
4. optionally starts the T9 bridge (`nd_t9_bridge_start` in `libneodct.so`) on its own
   pthread;
5. `open("/dev/tty2", O_RDWR)`, `fork` + **immediate** `execve("/bin/sh", {"/bin/sh",
   "-i"}, env)` with fds 0/1/2 dup'd from the tty and `FD_CLOEXEC` on everything else
   (`close_fds=True`), then `waitpid`;
6. stops the bridge, writes `\x1b[?25l`, `chvt <ui_vt>`, `usleep(50000)`.

Two C-specific hazards:

* **`fork()` in a threaded process.** The app child is already a separate process, but
  if the T9 bridge thread is running when the shell is spawned, the coding standard
  applies: fork then exec immediately, touch nothing in between (no `malloc`, no
  `printf`). Build the `argv`/`envp` arrays **before** the fork.
* **The environment.** `PS1=NeoDCT # ` and `TERM=linux` are added to a *copy* of the
  app's environment. `PS1` only takes effect because busybox `ash -i` reads it; keep the
  trailing space.

The app must not link the rasteriser at all — it draws nothing.

### 11. RemoteShell (`apps/RemoteShell/main.py`, 165 lines) — APP_ID 9990

Fully specified in `spec-storage-settings.md` §5 (lines 1238–1290). Summary of the parts
that belong to this survey:

```
APP_ID = 9990   TITLE = "Remote"   KEY_ENTER = 28
STATUS, TOGGLE, RELAY, LOGIN, PORT, KEYS, FINGERPRINT = range(7)
```

`TITLE` is `"Remote"` and not `"Remote Shell"` for a measured reason recorded in the
source: *"'Remote Shell' is 189px against 136 available; this is 111."* Do not
"improve" it.

`_menu_lines()` is rebuilt on **every** pass of the loop, because the list is also the
status display:

```python
state = RemoteShell.status();  current = RemoteShell.settings()
running = "On"       if (state["sshd"] and state["tunnel"]) else \
          "Dialling" if (state["sshd"] or  state["tunnel"]) else "Off"
return ["Status: %s" % running,
        "Turn off" if (state["sshd"] or state["tunnel"]) else "Turn on",
        "Relay: %s" % (current["host"] or "not set"),
        "Login: %s" % current["user"],
        "Port: %s"  % current["port"],
        "Copy keys from card",
        "This phone's key"]
```

The three-state `On` / `Dialling` / `Off` is deliberate: half-up means the relay refused
or dropped the tunnel, and `On` would be a lie.

Dispatch: `choice < 0` returns. `TOGGLE` (index 1) turns off when either process is
running, otherwise asks
`"Let this phone be reached over the internet?"` with button `Turn on` and calls
`_turn_on` only on ENTER. `RELAY`/`LOGIN`/`PORT` open `TextInput(ui, "Remote", prompt,
initial_text=…)` with `input_filter` `"any"` / `"letters"` / `"numbers"` respectively;
`None` (cancel) leaves the setting untouched, otherwise
`RemoteShell.save_settings(host=|user=|port=…)`. Prompts are exactly `"Relay host:"`,
`"Login:"`, `"Relay port:"`. Indices 0 (`STATUS`) and any unmatched value do nothing.

`_copy_keys(ui)`: `card = Storage.MOUNT_POINT` (`/NeoDCT/User/sdcard`); if not a
directory, `"No card in the phone."`. On `RemoteShell.RemoteShellError` show
`str(exc)`. On success:
`"Copied: %s.\n\nDelete them from the card now -- anyone who takes the card out can read them." % ", ".join(sorted(taken))`
— note the ASCII double hyphen, not an em dash.

`_show_fingerprint(ui)`: `RemoteShell.ensure_host_key()` (errors shown as-is), then
`"This phone:\n%s" % (RemoteShell.host_fingerprint() or "unknown")`.

`_turn_on(ui)`: `RemoteShell.start()`, errors shown as-is, else
`"Remote Shell is on.\n\nIt stays on across restarts until you turn it off here."`
`_turn_off(ui)`: `RemoteShell.stop()` then `"Remote Shell is off."`.

Every dialog is `MessageDialog(ui, message, title="Remote", button_text="OK")`, except
the confirmation which uses `button_text=button` and compares the result to `28`.

The app also does the `sys.path.insert(0, _APP_DIR)` dance at import time (lines 14–16);
irrelevant in C.

### 12. Downgrade (`apps/Downgrade/main.py`, 160 lines) — APP_ID 9006

Fully specified in `spec-update-system.md`. The parts specific to this app:

```
APP_ID = 9006     HEADER = "Downgrade"     ENTER = 28
APP_ICON = "/NeoDCT/System/engineering/apps/Downgrade/icon.png"
NO_NETWORK = ("This tool reads the release list from GitHub, so the phone needs a "
              "working data connection.\n\n"
              "Without one, an older package can still be copied onto the card by "
              "hand and installed from Update.")
```

Flow, in order:

1. `platform = get_setting("system.os.platform", "unknown")`;
   `installed = get_setting("system.os.versionnumber", "") or ""`.
2. `ProgressScreen(ui, "Reading releases", header="Downgrade")` then `.draw(0, 1)`
   (0 %).
3. `releases = remote.all_releases(platform)`.
   * `remote.NoRelease` → `DetailPage(title="Nothing published",
     subtitle="for %s" % platform, body="No release carries a package for this phone yet.",
     image=APP_ICON, header="Downgrade", softkey_text="Back")` and return.
   * `remote.NetworkError as exc` → same page shape with
     `title="No connection"`, `subtitle="Could not reach GitHub"`,
     `body="%s\n\n%s" % (exc, NO_NETWORK)`.
4. Build labels — the running version is **marked, not hidden**:

```python
for entry in releases:
    mark = "  (running)" if entry["version"] == installed else \
           "  (older)"   if (installed and not remote.is_newer(entry["version"], installed)) else ""
    labels.append("%s%s" % (entry["version"], mark))
```

   (two leading spaces inside both marks).
5. `VerticalList(ui, "Releases", labels, app_id=9006)`, `SoftKeyBar(ui).update("Select",
   present=False)`, `choice = menu.show()`; `if choice < 0 or choice >= len(releases):
   return`.
6. Picking the running version → page `title="Already running"`,
   `subtitle="NeoDCT %s" % installed`, `body="That is the version this phone is running."`.
7. Going backwards asks a spelled-out question rather than a bare "are you sure":

```
"Go back to %s?\nThis replaces the whole system. User data is kept but stays as %s left it."
    % (picked["version"], installed)      button "Downgrade"
```

   Going forwards asks
   `"Install %s?\n%s" % (picked["version"], "%.1f MB" % (size / 1048576.0))`,
   button `Install`. Both are `MessageDialog(...).show() == 28`; anything else returns.
8. `folder = Storage.folder("update")` — `None` unless a card is mounted at
   `/NeoDCT/User/sdcard` with all of `wallpapers tones backup_db music update` present.
   `None` → `_refuse(ui, "The card has no update folder.")` (a `MessageDialog` with
   `button_text="OK"` and `cancel_keys=()`, so **only** ENTER dismisses it).
9. `destination = os.path.join(folder, remote.asset_name(platform))`.
10. `ProgressScreen(ui, "Downloading %s" % picked["version"], header="Downgrade")` and
    `remote.download(url, destination, size=picked["size"], progress=lambda done, total:
    progress.draw(done, total or picked["size"] or 1))`. `UpdateError` →
    `_refuse(ui, "Download failed.\n%s\n\nNothing was installed." % exc)`.
11. **Hands off to the Update app's installer rather than reimplementing it** — the
    source comment says why: *"One signature check, one staging path, one applier."*

```python
spec = importlib.util.spec_from_file_location("neodct_update_app",
                                              "/NeoDCT/System/apps/Update/main.py")
update_app = importlib.util.module_from_spec(spec); spec.loader.exec_module(update_app)
update_app._install(ui, destination)
```

    Any exception → `_refuse(ui, "Downloaded, but could not start the installer.\n%s\n\n"
    "The package is on the card; install it from Update." % exc)`.

**In C** step 11 becomes a call into `libndupdate.so`'s `nd_update_install()` — the
Update spec already hoists `_install` there for exactly this reason. Do not fork the
Update app.

The module docstring is worth carrying across as the header comment: it explains that an
update replaces the whole root filesystem, that going back is not "undo", and that
0.3.4a/0.3.5a leak QMI clients until the modem stops answering.

### 13. `tools/` — the command-line utilities

#### 13.1 `tools/atcmd` (busybox ash, 59 lines) — **keep, do not port**

Called on every boot by `/etc/init.d/S45modem` (line 39,
`ATCMD=/NeoDCT/System/engineering/tools/atcmd`), so it is not optional and it is not
engineering-only. It is already the smallest possible implementation.

```
PORT   ${MODEM_PORT:-/dev/ttyUSB2}       -p PORT overrides
TIMEOUT 6                                 -t SECONDS overrides
LOCK   /tmp/neodct-modem.lock             the same advisory lock ModemService uses
CMD    "$1" or "AT"

exit 0  reply ended in OK or CONNECT*
exit 1  bad usage, or $PORT is not a character device, or cannot open it
exit 2  timeout waiting for a final result
exit 3  port busy (lock held for TIMEOUT seconds)
exit 4  ERROR / +CME ERROR: / +CMS ERROR: / NO CARRIER / NO DIALTONE / BUSY / NO ANSWER
```

Mechanism: `exec 9>>"$LOCK"`, then if `flock` exists loop `flock -x -n 9` sleeping 1 s
per try until `tries >= TIMEOUT`; `stty -F "$PORT" 115200 raw -echo -echoe -echok
clocal`; `exec 3<>"$PORT"`; `printf '%s\r' "$CMD" >&3`; read lines with
`IFS= read -r -t "$LEFT" LINE <&3`, strip `\r` with `tr -d '\r'`, skip empties, echo each
line, and exit on the first final result code. Not usable for `AT+CMGS` (needs the `>`
prompt).

#### 13.2 `tools/consolei2ckeypadbuilder.py` (353 lines) — **essential, port it**

Solves the first-boot chicken-and-egg: `KeypadMapperI2C` lives inside the UI, and
navigating the UI needs a working keypad. This does the same capture over the serial or
ADB console and writes the byte-identical JSON.

```
DEFAULT_OUTPUT = "/NeoDCT/User/keymap.json"
--bus  (default DEFAULT_BUS = 3)    --addr (hex or decimal, default 0x20)
--rows --cols (comma lists, defaults [0,1,2,3] / [4,5,6,7])
--output --test --discover
```

`KEY_TARGETS` is the same 16 names as §8 with **longer labels** for the first two:
`("navikey","NaviKey (center/enter)")`, `("clear","C (clear/back)")`; the other 14
labels are identical. Those labels are written into the JSON `keys[*].label`, so a map
built by this tool differs from one built in the UI by those two strings.

`NAME_TO_CODE` (used only by `--test` to print the keycode):

```
navikey 28  clear 14  up 103  down 108  left 105  right 106  menu 50
num_1 2  num_2 3  num_3 4  num_4 5  num_5 6  num_6 7  num_7 8  num_8 9
num_9 10  num_0 11  star 42  hash 43
```

This must mirror `core/main.py:MATRIX_NAME_TO_CODE`, which additionally maps
`"enter"→28` and `"back"→14`.

Capture loop:

* `print(f"[{index+1:2d}/{len(order)}] press: {label:<24} ", end="", flush=True)` —
  two-space-padded index, label left-justified in a 24-column field.
* `_wait_press_or_command(scanner)` polls `scanner.scan_once()` and, when nothing is
  pressed, `_stdin_command()` (a `select.select([sys.stdin], [], [], 0)` non-blocking
  `readline().strip().lower()`), sleeping 0.01 s between passes. Recognised commands are
  `s` (skip, advance without recording), `r` (redo previous — index -= 1 and the
  previous entry is popped from both `captured` and `position_owner`; at index 0 prints
  `nothing to redo`), `q` (`quit -- nothing saved.`, returns `None`).
* A duplicate position prints
  `f"R{r}C{c} already mapped to '{owner}' -- press a different key"` then
  `_drain_keypad(scanner)` and retries.
* A good capture prints
  `f"-> R{r} C{c} (pins P{row_pins[r]}/P{col_pins[c]})"` then `_drain_keypad`.
* `_drain_keypad(scanner, seconds=0.4)` calls `scan_once()` every 0.02 s for 0.4 s so a
  held key cannot answer two prompts.
* Empty result → `no keys captured -- nothing saved.` and `None`.

`save()` writes the **v3 i2c** payload (identical shape to §9, with `"output"` set to
the `--output` path and `i2c_bus`/`i2c_addr` as ints), then prints
`f"\nsaved {len(captured)} keys -> {path}"`. `main()` then prints
`restart the NeoDCT UI (or reboot) to pick it up.` and the `--test` command line.

`--test` reads the JSON, builds `{(row,col): name}` from `payload["keys"]`, prints
`testing {path} ({n} keys). press keys; Ctrl-C to stop.` and then, per press, either
`f"R{r}C{c} -> {name} (keycode {code})"` or `f"R{r}C{c} -> UNMAPPED"`, polling every
0.005 s.

`--discover` is the clever one and is worth porting as-is. It drives each of the 16
expander pins low in turn and reads the other 15; a pressed key shorts one row pin to
one column pin, so its partner reads low:

```python
for drive in range(16):
    chip.write16(0xFFFF & ~(1 << drive))
    time.sleep(0.0005)
    v = chip.read16()
    for bit in range(16):
        if bit != drive and not (v >> bit) & 1:
            seen.add((min(drive, bit), max(drive, bit)))
chip.write16(0xFFFF)
```

New pairs since the last sweep are printed as
`f"  connection: P{a} <-> P{b}"` (plus `" (already known)"` when already in the running
set), with a 0.03 s pause per sweep. On Ctrl-C it prints
`f"\n{len(pairs)} distinct key connections observed."` and 2-colours the connection
graph (`_bipartition`, BFS with a stack, colours 0/1, conflicts recorded) to split the
pins into rows and columns:

```
suggested split (either assignment works):
  --rows a,b,c,d --cols e,f,g,h
next: python3 <argv0> --bus <bus> --rows ... --cols ...
```

Conflicts print `WARNING: connections conflict with a clean matrix split (e.g. <pair>).
A wire may be loose or two keys were pressed at once. Re-run and press one key at a
time.` No connections at all prints
`no connections seen -- check wiring/pull-ups and press harder.` and returns 1.

Both failure paths for scanner construction print
`f"scanner init failed: {exc}"` followed by
`"check: device exists (ls /dev/i2c-*), chip visible (i2cdetect -y {bus}), pull-ups on
SDA/SCL."`.

#### 13.3 `tools/max1704x_watch.py` (136 lines) — nice to have

Pure-stdlib console watcher for the fuel gauge. Register map and scaling — these
constants are duplicated in `BatteryService` and must agree:

```
I2C_SLAVE = 0x0703
REG_VCELL 0x02   REG_SOC 0x04   REG_MODE 0x06   REG_VERSION 0x08
REG_CONFIG 0x0C  REG_CRATE 0x16   (MAX17048/49 only; 0xFFFF on 17043/44)
VCELL_LSB = 78.125e-6     volts per LSB across the full 16 bits
CRATE_LSB = 0.208         %/hr per LSB, signed
quickstart writes 0x4000 to REG_MODE
vcell = raw * VCELL_LSB   soc = raw / 256.0   crate = signed16(raw) * CRATE_LSB
```

Register access is `os.write(fd, bytes([reg]))` then `os.read(fd, 2)`, big-endian —
exactly the raw transactions a `I2C_SLAVE` ioctl makes correct.

CLI: `--bus` (default **3**), `--addr` (default 0x36, parsed with `int(s, 0)`),
`--interval` (default 0.25), `--quickstart`, `--csv`. Human output:

```
MAX1704x @ 0x%02X bus %d  VERSION=0x%04X  CONFIG=0x%04X
(CRATE column only meaningful on MAX17048/49; garbage/0xFFFF means you have a 17043/44)
quick-start issued, SOC re-seeded from VCELL          (only with --quickstart, then sleep 0.2)
t=%7.2fs  VCELL=%.4f V  SOC=%6.2f %%  CRATE=%+7.2f %%/hr[  dV=%+6.1f mV]
...
sweep summary: VCELL min %.4f V, max %.4f V           (on Ctrl-C, preceded by a blank line)
```

CSV header: `t_s,vcell_v,soc_pct,crate_pct_hr,raw_vcell,raw_soc,raw_crate`; rows
`%.3f,%.4f,%.2f,%.2f,0x%04X,0x%04X,0x%04X`. `sys.stdout.flush()` after every sample. The
`dV` suffix is omitted on the first sample.

#### 13.4 `tools/mouse_shim.py` (123 lines) — dev-only, safe to drop

Creates a virtual **absolute** touch device (`ABS_X`/`ABS_Y` 0..239, `BTN_TOUCH`) named
`NeoDCT-Touch-Cursor` and drives it from a real USB keyboard so NetSurf can be operated
without a mouse. `WIDTH = HEIGHT = 240`, `STEP = 8`, cursor starts centred at (120, 120)
and is clamped to 0..239. `W/A/S/D` move, `M` taps (BTN_TOUCH 1, position, BTN_TOUCH 0),
`TAB` toggles between CURSOR mode (keyboard grabbed) and TYPING mode (ungrabbed), `C`
runs `os.system("killall netsurf-fb")` and exits.

It imports `evdev`, which is **not** in either defconfig, so it cannot run on a shipped
image at all. It is a desktop/QEMU convenience. The browser is explicitly out of scope
in `ARCHITECTURE.md`; drop this or port it last as `nd-mouse-shim`.

#### 13.5 `tools/debug_sms_seed_inbox.py` (114 lines) — keep as Python

Seeds `/NeoDCT/User/db/sms_inbox.db`. The schema it declares must stay byte-identical to
the one `Messages` creates (`spec-storage-settings.md` line 592 flags the duplication):

```sql
CREATE TABLE IF NOT EXISTS inbox
(id INTEGER PRIMARY KEY AUTOINCREMENT,
 message TEXT,
 sender TEXT,
 timestamp INTEGER,
 is_read INTEGER DEFAULT 0)
```

Defaults: `--db /NeoDCT/User/db/sms_inbox.db`, `--sender 555-1234`, `--count 50`,
`--words-per-message 5`. Messages are `" ".join(random.sample(WORDS, n))` from the
26-word NATO alphabet list (`alpha`…`zulu`), timestamps are `int(time.time()) + idx`,
`is_read = 0`. Logging format is `[SMS SEED] %(message)s` at INFO.

#### 13.6 `tools/debug_phonebook_createrandomcontacts.py` (231 lines) — keep as Python

Fetches `https://www.thenamegeek.com/most-common-first-names` over HTTPS, parses the
table with a stdlib `HTMLParser` subclass, keeps rows whose column 0 is a digit and
whose column 1 matches `[A-Za-z][A-Za-z'\-]*`, deduplicates case-insensitively, excludes
first names already in the database, shuffles unless `--no-shuffle`, and inserts
`--count` (default **900**) rows of `(name, "555-%04d", NULL)` into:

```sql
CREATE TABLE IF NOT EXISTS contacts
(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, number TEXT, speed_dial INTEGER)
```

`--truncate` deletes every existing contact first. It needs network access and an HTML
parser; there is no case for putting either in the C image. Leave it in Python, run it
from a host against a mounted user partition.

### 14. Ranking — how essential each piece is to a 1:1 port

| Rank | Piece | Why |
| --- | --- | --- |
| 1 | **Engineering-mode gate** (§1) | Not an app. Without it none of the others appear, the menu order is wrong, and the Settings toggle and the Update unsigned-install path both break. Port with the core. |
| 2 | **`tools/atcmd`** | Runs on every boot from `S45modem`. Not optional. Zero work: keep the shell script byte-for-byte. |
| 3 | **`tools/consolei2ckeypadbuilder.py`** | The only way to bring up a keypad on a phone whose keypad does not work yet. A phone with no keymap cannot reach the UI to run the in-UI mapper. |
| 4 | **KeypadMapperI2C** | The supported in-UI enrolment path, and the one that actually functions on the device. Shares the JSON writer with #3. |
| 5 | **Crash + the crash screen** | The crash screen is core behaviour (`spec-core-loop.md` depends on it) and one of the 49 golden frames. The app itself is 17 lines and is the only way to exercise it on-device. |
| 6 | **LinuxShell** | The project's stated debugging story gets *worse* in C (`ARCHITECTURE.md`, "What this costs you"). An on-device root shell is the mitigation. Also cheap: it draws nothing. |
| 7 | **Modem** | The documented way to diagnose "no signal" without a serial cable, and the reason it does not bail out in simulation mode. Largest of the display apps. |
| 8 | **FuelGauge** | Bench instrument for battery work; a golden frame depends on it. Small once `nd_battery` exposes `debug_snapshot`. |
| 9 | **LCDTest** | Two golden frames' worth of value for ~110 lines, and it is the panel bring-up test. |
| 10 | **RemoteShell app** | Already specified and scheduled in `spec-storage-settings.md`; the core service is the real work. |
| 11 | **Downgrade** | Already specified and scheduled in `spec-update-system.md`; needs only `nd_update_install()` to exist. |
| 12 | **CubeBench** | Pure benchmark — but it is the *only* app in the tree that measures frame rate, which is the number the whole rewrite is being judged on. Port it early for that reason even though nothing depends on it. |
| 13 | **TestsApp** | 31 lines, one golden frame, exercises `MessageDialog`. Free. |
| 14 | **`tools/max1704x_watch.py`** | Superseded on-device by FuelGauge; still the tool for a bench sweep with CSV output. |
| 15 | **KeypadMapper (GPIO)** | Cannot function on any shipped image (no gpiozero). Port the two dialogs only, or delete the app and its manifest. |
| 16 | **`tools/debug_sms_seed_inbox.py`, `tools/debug_phonebook_createrandomcontacts.py`** | Host tools. Leave in Python. |
| 17 | **`tools/mouse_shim.py`** | Needs `evdev`, which is not in the image; serves the browser, which is out of scope. Drop. |

---

## Public interface (the functions other parts call)

### What the rest of the system calls *into* this subsystem

Only one entry point per app, and it is the app ABI, not something specific to
engineering:

```c
/* apps/<Name>/app.so */
int app_run(nd_ui *ui);        /* the C equivalent of Python's run(ui) */
```

`nd-apprun` `dlopen()`s the `.so`, resolves `app_run`, calls it once, and exits with its
return value. Nothing in the tree calls an engineering app by any other name, and no
engineering app exports anything else. (`Downgrade` calls *into* the Update app, never
the reverse; `KeypadMapper`/`KeypadMapperI2C` export nothing.)

### What this subsystem calls *out* to

Grouped by the C module that must provide it.

**Core / `nd_ui` (the app-side handle):**

```c
int32_t  nd_ui_width(const nd_ui *);                 /* 240 */
int32_t  nd_ui_height(const nd_ui *);                /* 175 */
int32_t  nd_ui_softkey_h(const nd_ui *);             /*  30 */
int32_t  nd_ui_content_bottom(const nd_ui *);        /* 145 */
nd_font *nd_ui_font(const nd_ui *, nd_font_size);    /* s=14 md=18 n=20 xl=24 */
void     nd_ui_text_size(const nd_ui *, const char *, nd_font *, int32_t *w, int32_t *h);
nd_image*nd_ui_canvas(nd_ui *);
void     nd_ui_present(nd_ui *);                     /* ui.fb.update(ui.canvas) */
int32_t  nd_ui_read_key(nd_ui *, double timeout_s);  /* -1 == none; 0 timeout = poll */
int32_t  nd_ui_wait_key(nd_ui *);                    /* blocks */
nd_image*nd_ui_get_image(nd_ui *, const char *path, int32_t max_size);
```

**Rasteriser (`libneodct.so`):** `nd_draw_rect_fill`, `nd_draw_line`, `nd_draw_text`,
`nd_image_paste` (with alpha), `nd_image_open`, `nd_image_resize` (LANCZOS).
Nothing here needs polygon, ellipse or point.

**UI framework (`libneodct.so`):**

```c
nd_softkey *nd_softkey_new(nd_ui *);
void        nd_softkey_update(nd_softkey *, const char *text, bool present);
int32_t     nd_msgdialog_show(nd_ui *, const char *msg, const nd_msgdialog_opts *);
int32_t     nd_vlist_show(nd_ui *, const char *title, const char *const *items,
                          size_t n, int32_t app_id, int32_t *selected_io);
const char *nd_textinput_show(nd_ui *, const char *title, const char *prompt,
                              const char *initial, nd_input_filter);
int32_t     nd_detailpage_show(nd_ui *, const nd_detailpage_opts *);
nd_progress*nd_progress_new(nd_ui *, const char *step, const char *header);
bool        nd_progress_draw(nd_progress *, int64_t done, int64_t total);
```

`nd_msgdialog_opts` must carry `title`, `icon_path`, `button_text`, `accept_keys`,
`cancel_keys`, `margin` with the Python defaults (`button_text="OK"`,
`accept_keys={28}`, `cancel_keys={14}`, `margin=8`, icon
`/NeoDCT/System/ui/resources/img/errorscreen/warning.png`), because Downgrade's
`_refuse()` relies on `cancel_keys=()` and RemoteShell relies on `title=`.

**Battery (`nd_battery`, in the core, reached over the app channel):**

```c
bool nd_battery_hardware(void);
bool nd_battery_debug_snapshot(nd_batt_snapshot *out);   /* false == simulation */
bool nd_battery_quickstart(void);
```

`nd_batt_snapshot` fields, matching the Python dict exactly:
`bus, addr, version, level, has_smoothed, smoothed_v, has_error, error[N],
raw_vcell, vcell, raw_soc, soc, raw_crate, has_crate, crate, config`.

**Modem (`nd_modem`, in the core, reached over the app channel):**

```c
bool nd_modem_status_snapshot(nd_modem_snapshot *out);
/* fields: hardware, port[], imei[], state[], csq(int or absent), bars(int or absent),
           reg_stat(int or absent), registered, operator[], caller_id[] */
nd_err nd_modem_send_at(const char *cmd, double timeout_s,
                        char *final_out, size_t final_sz,
                        nd_strlist *lines_out);          /* the (final, lines) tuple */
```

`send_at` must keep using the `/tmp/neodct-modem.lock` flock so `atcmd`, `S45modem` and
the UI polling never collide.

**Storage / settings (`libneodct.so`):** `nd_settings_get`, `nd_settings_set`,
`nd_storage_folder("update")`, `nd_storage_mount_point()`.

**Remote shell (`nd_remoteshell.c`, core):** `status`, `settings`, `save_settings`,
`install_keys_from_card`, `ensure_host_key`, `host_fingerprint`, `start`, `stop` — all
named in `spec-storage-settings.md`.

**Update (`libndupdate.so`):** `nd_remote_all_releases`, `nd_remote_is_newer`,
`nd_remote_asset_name`, `nd_remote_download`, `nd_update_install`.

**Keypad (`libneodct.so` + core):** `nd_pcf8575_open/close/read16/write16`,
`nd_matrix_scan_once`, and the new `nd_keymap_write()` below.

### New shared function this subsystem needs

```c
/* libneodct.so — nd_keymap_write.h
 * Writes the keymap JSON exactly as json.dump(indent=2, sort_keys=True) does,
 * plus one trailing newline. Used by KeypadMapper, KeypadMapperI2C and
 * nd-keymap-console, all three of which currently duplicate it.            */
typedef struct {
    const char *name;      /* "navikey" ... */
    const char *label;     /* "NaviKey" ... */
    int32_t row, col;
    int32_t row_pin, col_pin;
} nd_keymap_entry;

nd_err nd_keymap_write(const char *path,
                       const char *format,     /* "neodct.keymap.v2.matrix" | v3 */
                       const char *driver,     /* "gpiozero-matrix" | "pcf8575-i2c" */
                       int64_t generated_at_unix,
                       const int32_t *row_pins, size_t n_rows,
                       const int32_t *col_pins, size_t n_cols,
                       bool has_i2c, int32_t i2c_bus, int32_t i2c_addr,
                       const nd_keymap_entry *keys, size_t n_keys);
```

---

## External dependencies and their C replacements

| Dependency | Used for | C replacement |
| --- | --- | --- |
| `PIL.ImageDraw.rectangle` | every screen's clear + LCDTest patterns | `nd_draw_rect_fill`, **inclusive** corners |
| `PIL.ImageDraw.line` | CubeBench edges, divider rules in FuelGauge/Modem/mappers | `nd_draw_line`, Bresenham, inclusive endpoints, `width=1` |
| `PIL.ImageDraw.text` | all labels | `nd_draw_text` (FreeType, same `font.ttf`, sizes 14/18/20/24) |
| `PIL.ImageDraw.textbbox` | `ui.get_text_size` | `nd_ui_text_size`, must return **ink extents** `(x1-x0, y1-y0)` |
| `PIL.Image.open` + `.convert("RGB")` + `.resize(LANCZOS)` + `canvas.paste` | crash screen image, app icons | `nd_image_open`, `nd_image_resize` (LANCZOS), `nd_image_paste` with alpha |
| `Image.thumbnail(LANCZOS)` | `ui.get_image(max_size=…)` icon cache | `nd_ui_get_image` with the same 32-entry FIFO cache and `"<path>@<max>"` key |
| `time.monotonic` | FuelGauge and Modem 1 Hz refresh | `clock_gettime(CLOCK_MONOTONIC)` as `double` |
| `time.perf_counter` | CubeBench dt/FPS | same clock; **the golden harness must substitute the virtual clock** |
| `time.time` | keymap `generated_at_unix`, uinput event stamps, crash log | `time(NULL)` / `gettimeofday` |
| `time.sleep` | 1 ms row settle, 10 ms poll, 50 ms VT settle, 200 ms uinput bind | `nanosleep` |
| `math.cos` / `math.sin` | CubeBench rotations | `cos()` / `sin()` from libm, **`double`** |
| `json.load` | manifests, `--test` keymap read | small hand-rolled parser or the one `nd_manifest` already needs |
| `json.dump(indent=2, sort_keys=True)` | keymap writer | `nd_keymap_write` (above) — ASCII key order, 2-space indent |
| `os.makedirs(exist_ok=True)` | keymap dir, crash log dir | `mkdir()` walking the path, ignoring `EEXIST` |
| `os.listdir` | `/dev` ttyUSB scan, `/sys/class/net` | `opendir`/`readdir` + `qsort` for the sorted variants |
| `os.path.realpath` | resolving `…/device/driver` to `qmi_wwan` | `realpath(3)` |
| `open()` + read | `/proc/uptime`, `/proc/meminfo`, `/proc/net/if_inet6`, `/sys/class/net/*/flags`, `/etc/default/modem`, `/etc/resolv.conf`, `/tmp/modem.status` | `open`+`read` into a fixed buffer; every one of these is small and bounded |
| `socket.inet_ntop(AF_INET6, …)` + `bytes.fromhex` | `_global_ipv6` | `inet_ntop(AF_INET6, …)` after hex-decoding the 32-char field |
| `glob.glob("/dev/gpiochip*")`, `glob("/dev/i2c-*")` | the two mapper gates | `opendir("/dev")` + `strncmp` prefix test |
| `fcntl.ioctl(fd, 0x0703, addr)` | PCF8575 and MAX1704x slave select | `ioctl(fd, I2C_SLAVE, addr)` — same value |
| `os.write`/`os.read` on the i2c fd | register access | `write()`/`read()`, big-endian assembly by hand |
| `gpiozero.Button` / `OutputDevice` | KeypadMapper only | **absent from the image** — see Risks. Either reproduce the two error dialogs, or implement `/dev/gpiochip*` `GPIO_V2_GET_LINE_IOCTL` + `GPIO_V2_LINE_GET_VALUES_IOCTL` |
| `shutil.which` | finding `chvt` | `PATH` walk with `access(X_OK)` |
| `subprocess.run(..., DEVNULL)` | `chvt` | `posix_spawn` + `waitpid`, fds to `/dev/null` |
| `subprocess.Popen(["/bin/sh","-i"], stdin/out/err=tty)` | the shell itself | `fork` + **immediate** `execve`, `dup2` the tty onto 0/1/2 |
| busybox `chvt` | VT switching | external binary, unchanged |
| kernel `fbcon` | the shell's rendering | unchanged — nothing to write |
| `/dev/uinput` ioctls | T9 bridge | `nd_uinput.c` (`spec-hw-input.md` owns it); ioctl numbers `0x40045564`, `0x40045565`, `0x5501`, `0x5502`, bus `0x06` |
| `threading.Thread(daemon=True)` | T9 bridge poll loop | `pthread_create` + a `stop` flag; join with a 2 s timeout equivalent |
| `importlib.util.spec_from_file_location` | Downgrade loading Update's `_install` | `nd_update_install()` in `libndupdate.so` |
| `sqlite3` | the two seed tools | not needed on-device; those tools stay Python |
| `urllib.request` + `html.parser` | contacts seeder | not needed on-device; stays Python |
| `evdev` (`UInput`, `list_devices`, `grab`) | `mouse_shim.py` | not in the image; drop the tool |
| `argparse` | five console tools | `getopt_long` |
| `select.select` on stdin | console builder's `s`/`r`/`q` | `poll()` on fd 0 with timeout 0 |
| `random.sample`, `random.shuffle` | seed tools | not needed on-device |

---

## Proposed C modules

| File | Contents | Est. LOC |
| --- | ---: | ---: |
| `lib/nd_keymap_write.c` / `.h` | the `sort_keys=True, indent=2` keymap JSON writer shared by both mappers and the console builder | 130 |
| `lib/nd_text_wrap.c` / `.h` | the greedy `_wrap_text` used by both mappers (the framework wants it anyway) | 45 |
| `core/nd_engmode.c` / `.h` | `_setting_is_enabled`, the engineering app-dir list, the runtime add/remove-and-resort used by the Settings toggle, and the app→core "engineering mode changed" message | 110 |
| `apps/Crash/app.c` | one-item `VerticalList` then a deliberate fault | 45 |
| `apps/TestsApp/app.c` | softkey, dialog, centred "Hello World", the two-press exit | 60 |
| `apps/LCDTest/app.c` | four patterns, the TV bar geometry, 28/14 handling | 115 |
| `apps/CubeBench/app.c` | vertex/edge tables, three rotations, projection, FPS window, HUD | 190 |
| `apps/FuelGauge/app.c` | gate, 1 Hz loop, `_rows_from_snapshot` formatting, flash line | 175 |
| `apps/Modem/app.c` | page loop, `_draw_page`, `_radio_rows`, `_sim_rows` (the six AT queries), `_shorten`, `_first_content` | 300 |
| `apps/Modem/modem_probe.c` / `.h` | `_wwan_interface`, `_iface_up`, `_global_ipv6`, `_configured_apn`, `_dns_row`, `_read_file`, `_listdir` | 200 |
| `apps/LinuxShell/app.c` | `chvt`, tty writes, T9 bridge start/stop, fork+exec `/bin/sh -i`, teardown | 175 |
| `apps/KeypadMapper/app.c` | GPIO gate + the two dialogs; optionally the `/dev/gpiochip` scanner and the full wizard | 90 (330 with a real scanner) |
| `apps/KeypadMapperI2C/app.c` | I²C gate, config parsing, prompt drawing, 16-target capture, `matrix_input` suspend/restore | 300 |
| `apps/RemoteShell/app_remoteshell.c` | seven-line menu, three text inputs, six dialogs — *already budgeted in `spec-storage-settings.md`* | (230) |
| `apps/Downgrade/app.c` | release list, labels, two confirmations, download, hand-off — *already budgeted in `spec-update-system.md`* | (210) |
| `tools/nd_keymap_console.c` | console capture, `--test`, `--discover` + `_bipartition`, all the printed strings | 380 |
| `tools/nd_fuelwatch.c` | MAX1704x raw i2c watcher, human + CSV output | 190 |
| `tools/atcmd` | **unchanged busybox ash** | (59, no work) |
| `tools/mouse_shim.py`, `debug_*.py` | **unchanged Python**, host/dev only | (0) |
| **New C in this subsystem** | (excluding the two already budgeted elsewhere) | **≈ 2,505** |
| **Including RemoteShell + Downgrade** | | **≈ 2,945** |

Link map:

```
apps/Crash/app.so        -> libneodct
apps/TestsApp/app.so     -> libneodct
apps/LCDTest/app.so      -> libneodct
apps/CubeBench/app.so    -> libneodct, libm
apps/FuelGauge/app.so    -> libneodct                (battery over the app channel)
apps/Modem/app.so        -> libneodct                (modem over the app channel)
apps/LinuxShell/app.so   -> libneodct                (only for nd_t9_bridge; no rasteriser)
apps/KeypadMapper*/app.so-> libneodct
apps/RemoteShell/app.so  -> libneodct
apps/Downgrade/app.so    -> libneodct, libndupdate
tools/nd-keymap-console  -> libneodct (nd_pcf8575, nd_matrix, nd_keymap_write) only
tools/nd-fuelwatch       -> nothing but libc
```

`nd-keymap-console` and `nd-fuelwatch` must **not** pull in the rasteriser or the UI
framework — they run from a serial console with no framebuffer, and one of them runs on
a phone whose UI cannot start.

---

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| **`gpiozero` is not in either defconfig**, so `KeypadMapper` can only ever show one of two error dialogs on a real image. A C port that implements a working GPIO scanner is a *feature addition* disguised as a port, and it changes what the app does on the Luckfox (where `/dev/gpiochip*` exists) from "gpiozero is missing" to a live wizard. | high | Decide explicitly before writing code. Default to reproducing the two dialogs exactly (45 lines); if a live scanner is wanted, raise it as a deliberate deviation the way `ARCHITECTURE.md` raises the modem-thread change. |
| **`KeypadMapperI2C` sets `ui.matrix_input = None` for the duration of the capture**, so on keypad-only hardware the MENU-key cancel becomes unreachable and the operator's only way out is a power cycle. | medium | Reproduce it (it is current behaviour) but note it; the fix, if wanted, is to route the cancel through the scanner the app already owns. |
| **The capture prompt silently clips**: only four of the six body lines are ever drawn, so the pin numbers and "Menu key cancels." are invisible. Someone porting from the *intent* rather than the *code* will "fix" this and break the frame. | medium | The clipping rule is `if y > content_bottom - line_h: break`, i.e. `y > 126`, with `line_h = 19` and `y` starting at 56. Written out line by line in §8. |
| **CubeBench's golden frame depends on exact double arithmetic** through 240 iterations of `ax += 1.30 * dt` where `dt == 0.09999990463256836`. Any change — `float` instead of `double`, fused multiply-add, `-ffast-math`, a different clock formula — moves the cube. | high | Pin `double` everywhere in the cube math, forbid `-ffast-math` for that translation unit, and implement the virtual clock as `epoch + frame*tick` subtraction. The expected final angles are in §5.1; assert them in a unit test before comparing pixels. |
| **`ui.get_text_size` returns ink extents, not advance width.** A C helper that returns the advance width (the obvious FreeType answer) shifts every centred string in this subsystem by a few pixels. | high | One shared `nd_ui_text_size` built on the same bbox definition, unit-tested against the table in §0.1. |
| **`draw.rectangle` is inclusive of both corners.** Half-open rectangles put the LCDTest red one row short and shift both stripe blocks. | high | §0.3, plus the `eng-lcdtest` golden as the check. |
| **The Settings toggle mutates the core's in-memory app list**, which is impossible once Settings is a separate process. | medium | Add an app→core control message ("engineering mode is now X, rescan") to the IPC design, or have the core re-read the setting and rescan after every app exit. Either way it must take effect without a reboot. |
| **`atcmd` is on the boot path.** Treating `engineering/tools/` as "port last, or drop" would delete a file `S45modem` calls by absolute path on every boot, and the modem would never dial. | high | Keep the script verbatim; it is already listed as such in `spec-core-services.md` line 111. Add a build-time check that the path exists. |
| **FuelGauge's golden frame is a stub artefact**, not real behaviour: it renders the string `'NoneType' object cannot`. A C harness that stubs the battery differently produces a different — and arguably more correct — frame that will not match. | medium | Make `nd-shoot` reproduce the same state: `hardware = true` with no usable fd, and an error string that begins with those 24 characters. Or re-cut that one golden with a documented C-side stub and record the change. |
| **`_shorten` cuts on bytes, not characters.** The Python slices a `str`, so a multi-byte character can never be split; naive C byte slicing can produce invalid UTF-8 in the ICCID/IPv6/APN rows. | low | Slice on code points in `nd_shorten`; in practice all these fields are ASCII, so this only matters for a hostile APN string. |
| **`_first_content` and the `+CNUM` parse trust modem output.** `line.split('"')[3]` on a line with exactly four quotes is fine in Python (it raises otherwise, and `count('"') >= 4` guards it), but the C equivalent must bound every copy. | medium | Fixed-size buffers with `snprintf`, explicit quote counting, and a length check before every copy — coding standard §1.4. |
| **LinuxShell has no failure feedback.** If `chvt` is missing or the VT switch fails the app returns silently and the user sees the menu again with no explanation. | low | Preserve it (it is deliberate — the comment says keeping the UI responsive beats risky ioctls), but log to the console. |
| **`fork()` with the T9 bridge thread running.** The bridge is started before the shell is spawned, so the app process is multi-threaded at the moment it forks — the exact hazard `CODING-STANDARDS.md` §1.1 names. | high | Build `argv`/`envp` before the fork; between `fork()` and `execve()` call nothing but `dup2`, `close` and `execve`. |
| **Two copies of `_wrap_text` and two copies of `_setting_is_enabled`** in the Python. Porting each in place perpetuates the duplication. | low | Hoist both into `libneodct.so` on the way across; they are byte-identical. |
| **Keymap JSON must stay readable by `core/main.py:_load_matrix_keymap`**, which needs `row_pins`, `col_pins`, `keys` (each with int `row`/`col`), and for the i2c driver an `i2c_bus` and a decimal `i2c_addr`. | high | Round-trip test: write with `nd_keymap_write`, read with the core loader, compare the resulting `(row,col) → keycode` table. |
| **Menu order is a hidden dependency.** Several golden frames and any scripted key sequence assume the 24-entry order in §1.2. Getting an `id` wrong moves everything after it. | medium | Assert the full ordered name list in a unit test. |

---

## Tests that cover this

### Direct coverage (in `neodct/tests/`)

| File | What it covers | Usable as a port oracle? |
| --- | --- | --- |
| `test_linuxshell_t9.py` (75 lines, 3 tests) | Loads `apps/LinuxShell/main.py` **by file path** (the comment notes engineering apps are not packages) and tests `_start_t9_bridge` in isolation: `None` when `ui.matrix_input is None`; `None` when the `UInputKeyboard` constructor raises `PermissionError`; and a started bridge whose `_thread.is_alive()` and whose `stop()` closes the keyboard. | **Yes** — three clean behavioural assertions on the only branchy part of LinuxShell. Port them first. |
| `test_uistub.py::test_engineering_apps_are_hidden_when_engineering_mode_is_off` | `StubUI(engineering=False)` must not list `"LCD Test"`. | **Yes** — the gate in one line. |
| `test_remoteshell.py` (37 tests) | The `System/core/RemoteShell` service, not the app: sshd config contents, key permissions, tunnel command quoting, stale-PID handling, card key import. | Indirectly — the app is a thin driver over these. `spec-storage-settings.md` owns them. |
| `test_update_remote.py` (~40 tests) | `remote.all_releases`, `is_newer`, `version_key`, `asset_name`, `download` with resume/backoff — everything Downgrade calls. | Indirectly; `spec-update-system.md` owns them. |
| `test_post_build_prune.py::test_the_engineering_apps_are_pruned_the_same_way` | A directory under `NeoDCT/System/engineering/apps` that is not in the overlay is deleted from the target. | Yes, for the build script (which does not change). |
| `test_settings_version_layering.py` | `system.ui.engineering_mode` round-trips through `settings.prop` and defaults to `"ON"` when absent. | **Yes** — pins the default. |
| `test_systemupdate_app.py` (4 engineering-mode tests) | Engineering mode is the only way past a bad signature, and the second confirmation never appears without it. | Yes, for the flag's semantics. |

**There is no test at all for:** Crash, TestsApp, LCDTest, CubeBench, FuelGauge, Modem,
KeypadMapper, KeypadMapperI2C, or any of the six `tools/` scripts. That is 1,400+ lines
of Python with zero unit coverage.

### The real oracle: golden frames

`neodct/tests/golden/` holds 49 reference PNGs captured from the Python build with
`neodct/tools/goldenframe.py`, with a `manifest.json` recording each frame's size and the
SHA-256 of its **raw RGB bytes prefixed by `b"%d,%d|" % (w, h)`** (not of the PNG file).
**Six of them belong to this subsystem:**

| Frame | sha256 (first 8) | Size | What it is |
| --- | --- | --- | --- |
| `eng-cubebench` | `a4729594` | 240 × 175 | frame 240 of the cube animation under the virtual clock |
| `eng-fuelgauge` | `4e7286e2` | 240 × 175 | the `ERROR '<NoneType>…` readout, `i2c-3 @ 0x36`, `QStart` |
| `eng-lcdtest` | `73ffb08f` | 240 × 175 | the red flood fill, rows 0–144 |
| `eng-modem` | `f11f60c5` | 240 × 175 | RADIO page in simulation, 6 rows, `1/3`, `Next` |
| `eng-tests` | `8876fdd9` | 240 × 175 | the `MessageDialog` from TestsApp |
| `crash-screen` | `1381e6db` | 240 × 175 | `_draw_engineering_crash_screen(ui, "RuntimeError: example failure")` |

I re-ran the capture during this survey and confirmed
`python3 neodct/tools/goldenframe.py --compare neodct/tests/golden <fresh>` prints
`identical: 49 frames match` on this machine with Pillow 12.3.0 / FreeType 2.14.3 — so
the reference set is genuinely reproducible and is a hard oracle, not an aspiration.

How they are produced (`shoot_docs.py:shoot_engineering_apps`, lines 130–146):

```python
cases = [("ModemInfo", [], "eng-modem"), ("FuelGauge", [], "eng-fuelgauge"),
         ("LCD Test", [], "eng-lcdtest"), ("Cube Bench", [], "eng-cubebench"),
         ("Tests", [], "eng-tests")]
for name, keys, slug in cases:
    with StubUI() as ui:                                    # no wallpaper
        ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")
        frames = run_app(ui, name, keys=keys)               # frame_budget = 240
        save_frame(frames, slug, out)                       # index -1: the LAST frame
```

`simulate_status` sets `ui.battery.hardware = True`, `ui.battery._level = 4`,
`ui.modem.signal_level = lambda: 4`, `ui.modem.operator_display = lambda: "Tello"` — and
leaves both drivers otherwise unopened. `run_app` stops an app by exhausting either the
key script (`ScriptExhausted`, a `BaseException` so app code catching `Exception` cannot
swallow it) or the 240-frame budget on `fb.update()`.

The C build needs an `nd-shoot` that reproduces all of this: the same stubbed battery and
modem, the same frame budget, the same key-exhaustion unwind, and the virtual clock
(`EPOCH = 1704112496.0`, `TICK = 0.1`, `SEED = 20240101`, `TZ=UTC`).

### Tests worth adding while porting

* `nd_keymap_write` round-trip against `_load_matrix_keymap`'s reader.
* The `_setting_is_enabled` truth table, all nine strings plus `None` and garbage.
* `_shorten`, `_first_content` and the `+CNUM` quote parse, against captured SIM7600
  replies.
* CubeBench's 240-iteration angle accumulation, asserted against the three doubles in
  §5.1 before any pixel is compared.
* The 24-entry menu order with engineering mode on and the 13-entry order with it off.
* `_wrap_text` against the widths table in §8, including the "long word is not broken"
  case.
* `atcmd`'s five exit codes, driven against a pty that replies with each final code.

---

## How this could be split across agents

The subsystem is unusually parallel: eleven apps, no shared state between them, and only
two shared helpers. The dependency graph is shallow.

**Blocking prerequisites** (none of these are ours to build):

```
libneodct rasteriser + nd_ui_text_size + SoftKeyBar/MessageDialog/VerticalList
  -> everything except LinuxShell and the two console tools
nd_pcf8575 + nd_matrix (spec-hw-input)         -> KeypadMapperI2C, nd-keymap-console
nd_battery debug_snapshot/quickstart           -> FuelGauge
nd_modem status_snapshot/send_at               -> Modem
nd_remoteshell (spec-storage-settings)         -> RemoteShell app
libndupdate + nd_update_install                -> Downgrade
nd_uinput + nd_t9_bridge (spec-hw-input)       -> LinuxShell
```

**Suggested split — six agents, roughly equal:**

| Agent | Scope | Depends on | Est. LOC |
| --- | --- | --- | ---: |
| **E1 — the gate and the freebies** | `nd_engmode.c`, `nd_text_wrap.c`, `apps/Crash`, `apps/TestsApp`, `apps/LCDTest`. Owns the menu-order test and two golden frames. | UI framework only | 375 |
| **E2 — the benchmark** | `apps/CubeBench` **plus** the virtual-clock hook in `nd-shoot` and the double-arithmetic test. Small in lines, high in precision; give it to whoever also builds the golden harness. | UI framework, libm | 190 + harness |
| **E3 — keypad enrolment** | `nd_keymap_write.c`, `apps/KeypadMapperI2C`, `apps/KeypadMapper`, `tools/nd-keymap-console`. One agent, because all four share the writer, the 16-target list and the prompt layout — splitting them guarantees three divergent JSON writers. | `nd_pcf8575`, `nd_matrix` | 900 |
| **E4 — instruments** | `apps/FuelGauge`, `apps/Modem`, `apps/Modem/modem_probe.c`, `tools/nd-fuelwatch`. Shares the MAX1704x register map between the app and the tool, and the AT-reply parsing is all in one head. | `nd_battery`, `nd_modem` | 865 |
| **E5 — the shell** | `apps/LinuxShell` and the `fork`/`execve`/VT discipline. Isolated: no rasteriser, no framework widgets, entirely process and tty work. Good candidate for the agent who also does `nd-apprun`, since it is the same skill. | `nd_uinput`, `nd_t9_bridge` | 175 |
| **(already assigned)** | `apps/RemoteShell` → the `spec-storage-settings.md` agent. `apps/Downgrade` → the `spec-update-system.md` agent. Do **not** duplicate them here. | — | (440) |

**Ordering.** E1 and E5 can start as soon as the framework and `nd_uinput` exist and are
the cheapest way to prove the app-process model end to end (E1 gives a menu entry, a
crash and a dialog; E5 gives a shell you can debug the rest from). E3 is on the critical
path for any real hardware bring-up and should not wait. E2 wants the golden harness
anyway. E4 is last, because it is the only one that needs two core services finished.

**Serialisation points.** Exactly three, and they are all small:

1. `nd_keymap_write` must land before any of E3's four consumers.
2. `nd_ui_text_size`'s ink-extent semantics must be settled before E1, E2 or E4 draw
   anything, or every centred string is wrong in three places at once.
3. The engineering-mode app→core message (E1) must be agreed with whoever owns the core
   IPC, because Settings lives in another process.

Everything else is independent, and each app's correctness is decided by a golden frame
or by a handful of pure-function unit tests, so agents can verify their own work without
waiting for each other.

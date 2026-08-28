# Storage, settings, crash handling, remote shell — C port specification

*Subsystem survey for the 1:1 Python → C rewrite. Everything here was read out of the
actual source; every constant, path, pixel offset and string is quoted from it.*

**Python in scope**

| File | LOC |
| --- | --- |
| `neodct/overlay/NeoDCT/System/core/SettingsStorage/__init__.py` | 121 |
| `neodct/overlay/NeoDCT/System/core/Storage/__init__.py` | 124 |
| `neodct/overlay/NeoDCT/System/core/CrashHandler/__init__.py` | 242 |
| `neodct/overlay/NeoDCT/System/core/ErrorScreen/__init__.py` | 76 |
| `neodct/overlay/NeoDCT/System/core/RemoteShell/__init__.py` | 487 |
| `neodct/overlay/NeoDCT/System/core/logstyle.py` | 182 |
| `neodct/overlay/NeoDCT/System/engineering/apps/RemoteShell/main.py` | 165 |
| `neodct/overlay/NeoDCT/System/core/main.py` — `init_databases()` (lines 441–496) | 55 |
| `neodct/overlay/NeoDCT/System/core/main.py` — `_store_incoming_sms()` / `_count_unread_sms()` | 25 |
| **Total** | **1477** |

Not Python, but part of this subsystem's contract and **staying as shell**:
`neodct/overlay/NeoDCT/System/hw/neodct-sdcard` (274 lines),
`neodct/overlay/etc/init.d/S00userdata`, `S06sdcard`, `S99banner`,
`neodct/overlay/etc/neodct-colors.sh`, `neodct/overlay/bin/run_neodct.sh`,
`neodct/scripts/post-build-system-metadata.sh`.

---

## What this does (plain English, for a reader who is not a C programmer)

This subsystem is the phone's **memory in the everyday sense** — the parts that remember
things between one boot and the next — plus the parts that deal with **things going
wrong**, plus the **back door for developers**.

There are five separate jobs in here, and they only sit together because they all touch
the writable partition.

### 1. Settings — the phone's preferences file

The phone keeps its preferences in one plain text file, `/NeoDCT/User/settings.prop`. It
looks exactly like you would expect:

```
games.snake.topscore=42
system.audio.ringtone=/NeoDCT/System/tones/Low.mp3
system.ui.engineering_mode=ON
system.ui.wallpaper=NONE
```

One `key=value` per line, sorted alphabetically. Any app can ask for a value
(`get_setting("system.ui.wallpaper")`) or store one (`set_setting(...)`).

There is a second file, `/NeoDCT/System/version.prop`, that says which version of the OS
this is. That one is **read-only** and is regenerated every time the image is built. The
reason they are separate is a bug that actually happened: the version used to live in
`settings.prop`, and `settings.prop` lives on a partition that *survives* a system
update — so after an update the phone would keep proudly reporting the version it
shipped with, forever. Now the version comes from a file that an update replaces, and it
always wins over anything stored in the preferences file.

There is a built-in table of defaults, so a brand-new phone with no preferences file at
all still knows it should use the "Low" ringtone and no wallpaper.

**One thing to know, because it is surprising:** reading a single setting currently
**rewrites the entire preferences file to flash**. Not once at boot — every single time.
This is an accident of how the defaults table is built (see "Behaviour that must be
reproduced exactly", W-1). It works, but it is slow and it wears the flash out. It is
flagged as an open question rather than silently fixed.

### 2. Storage — is there an SD card, and is it one of ours?

The phone has an SD slot. A shell script (`neodct-sdcard`) does the actual mounting when
a card is pushed in, and writes down what it found in a little status file at
`/run/neodct/sdcard.prop`. This module reads that file and turns it into one of four
answers the UI can act on:

- **absent** — no card
- **ready** — a NeoDCT card: mounted, with all five folders on it
- **needs_setup** — a card that mounts fine but has not been laid out for NeoDCT yet
- **unformatted** — there is a card, but nothing we can read on it

The five folders a NeoDCT card must have are `wallpapers`, `tones`, `backup_db`,
`music`, `update`. Only a **ready** card hands out paths, so an app can never start
writing into a card that is about to be reformatted.

### 3. The databases — contacts, texts, calls

Three SQLite database files live in `/NeoDCT/User/db/`:

- `phonebook.db` — your contacts (name, number, speed dial slot)
- `sms_inbox.db` — texts you received
- `sms_outbox.db` — texts you sent
- `call_log.db` — missed / received / dialled calls

They are created at boot if they are missing, and a brand-new phone gets one seeded
contact: "NeoDCT Support" on 555-1234, speed dial 2.

### 4. Crash handling — what you see when something breaks

Two different things can break, and they look different.

**An app breaks.** Today Python catches it, writes the details to
`/NeoDCT/User/logs/crash.log`, and shows you a screen. If Engineering Mode is on, that
screen is a full-screen crash picture with a one-line summary of the error strapped
across the top and a "Continue" softkey. If Engineering Mode is off, you just get a
polite dialog box saying "An application has crashed."

**In C this changes shape, but not appearance.** C has no exceptions. Instead, each app
runs as its own separate program, and when it dies the core process is told by the
operating system (via `waitpid()`) *that* it died and *how* — which signal killed it.
The core then draws exactly the same screen. The picture on screen must be
pixel-identical; only the plumbing behind it is different.

**The whole UI breaks.** Then the shell script that launched it paints a red screen on
the text console saying `CRITICAL SYSTEM FAILURE / CODE: <n>`. That script does not
change.

### 5. Remote Shell — getting into the phone over the internet

Developing on this phone normally means a serial cable soldered to two pads, and those
wires come off. Remote Shell is the replacement: the phone dials out to a server you own
(a "relay"), and drags a way back in behind it. You then log in to the relay and hop
through to the phone.

It is off unless somebody turns it on. The phone's own SSH server only listens on
`127.0.0.1` — the loopback address, which nothing outside the phone can reach — so the
*only* route in is the tunnel the phone itself dialled. Keys only, no passwords, both
directions, and the phone checks the relay is really the relay before handing it
anything.

The keys arrive on the SD card (the phone has no keyboard), get copied onto the user
partition with tight permissions, and the operator is told to wipe the card.

The whole of this in C is: write two text files, launch two programs, remember two
process IDs, and be very careful about killing the right thing.

---

## Files and where they go in C

| Python file | Purpose | C destination |
| --- | --- | --- |
| `System/core/SettingsStorage/__init__.py` | key/value settings + version layering | `libneodct.so`: `nd_settings.c` / `nd_settings.h`, on top of `nd_props.c` |
| `System/core/Storage/__init__.py` | SD-card state as the UI sees it | `libneodct.so`: `nd_storage.c` / `nd_storage.h` |
| `System/core/main.py` `init_databases()` + `_store_incoming_sms()` + `_count_unread_sms()` | sqlite schema creation, seeding, SMS insert/count | `libneodct.so`: `nd_db.c` / `nd_db.h` |
| `System/core/CrashHandler/__init__.py` | crash log + crash screen | core binary: `nd_crash.c` / `nd_crash.h`; the crash-report writer half goes in `nd-apprun` (`apprun_crash.c`) |
| `System/core/ErrorScreen/__init__.py` | modal warning/notice wrappers | `libneodct.so`: `nd_errscreen.c` / `nd_errscreen.h` |
| `System/core/RemoteShell/__init__.py` | sshd + `ssh -R` tunnel management | core binary: `nd_remoteshell.c` / `nd_remoteshell.h` |
| `System/core/logstyle.py` | 256-colour serial log tags | `libneodct.so`: `nd_log.c` / `nd_log.h` |
| `System/engineering/apps/RemoteShell/main.py` | the on-phone switch/address book | app: `apps/RemoteShell/app.so` — `app_remoteshell.c` |
| `System/hw/neodct-sdcard` (shell) | mounts cards, publishes state | **unchanged**, stays a busybox `ash` script |
| `bin/run_neodct.sh` (shell) | launches the UI, paints the tty0 death screen | **unchanged** apart from `python3 /NeoDCT/launcher.py` → `/NeoDCT/System/bin/nd-core` |
| `scripts/post-build-system-metadata.sh` | writes `version.prop` at build time | **unchanged** |
| `etc/neodct-colors.sh` | the same palette for shell scripts | **unchanged** |

New C-only files with no Python original:

| File | Why |
| --- | --- |
| `nd_props.c` / `nd_props.h` | the three prop-file dialects and the atomic writer, shared by settings, storage and remoteshell |
| `nd_paths.h` | every absolute runtime path as a `#define`, in one place |

---

## Behaviour that must be reproduced exactly

### A. Paths (all absolute; `AGENTS.md` says these are load-bearing)

```
/NeoDCT/User/settings.prop                              SettingsStorage.SETTINGS_PATH
/NeoDCT/User/settings.prop.tmp                          atomic-write temp
/NeoDCT/System/version.prop                             SettingsStorage.VERSION_PATH  (read-only image)
/NeoDCT/User/sdcard                                     Storage.MOUNT_POINT
/run/neodct/sdcard.prop                                 Storage.STATE_FILE
/NeoDCT/User/db                                         sqlite directory
/NeoDCT/User/db/phonebook.db
/NeoDCT/User/db/sms_inbox.db
/NeoDCT/User/db/sms_outbox.db
/NeoDCT/User/db/call_log.db
/NeoDCT/User/logs                                       CrashHandler.CRASH_LOG_DIR
/NeoDCT/User/logs/crash.log                             CrashHandler.CRASH_LOG_PATH
/NeoDCT/User/logs/crash.log.1                           CrashHandler.CRASH_LOG_ROTATED
/NeoDCT/System/ui/resources/CRASH.jpg                   CrashHandler.CRASH_IMAGE_PATH (240x175 RGB JPEG)
/NeoDCT/System/ui/resources/img/errorscreen/warning.png ErrorScreen.DEFAULT_WARNING_ICON (24x24 RGBA PNG)
/NeoDCT/User/.ack_security_warning                      ErrorScreen.DEFAULT_ACK_FILE
/NeoDCT/User/.remote                                    RemoteShell.USER_DIR              (mode 0700)
/NeoDCT/User/.remote/state.prop                         RemoteShell.STATE_FILE
/NeoDCT/User/.remote/sshd_config                        RemoteShell.SSHD_CONFIG           (mode 0600)
/NeoDCT/User/.remote/ssh_host_ed25519_key               RemoteShell.HOST_KEY              (mode 0600)
/NeoDCT/User/.remote/ssh_host_ed25519_key.pub           (written by ssh-keygen)
/NeoDCT/User/.remote/authorized_keys                    RemoteShell.AUTHORIZED_KEYS       (mode 0600)
/NeoDCT/User/.remote/relay_id_ed25519                   RemoteShell.RELAY_KEY             (mode 0600)
/NeoDCT/User/.remote/known_hosts                        RemoteShell.KNOWN_HOSTS           (mode 0600)
/NeoDCT/User/.remote/tunnel.sh                          RemoteShell.TUNNEL_SCRIPT         (mode 0700)
/NeoDCT/User/.remote/tunnel.pid                         RemoteShell.TUNNEL_PID
/NeoDCT/User/.remote/sshd.pid                           RemoteShell.SSHD_PID (also sshd's own PidFile)
/NeoDCT/User/.remote/remote.log                         RemoteShell.LOG_FILE
/etc/neodct-banner                                      logstyle.banner_lines() default
```

`S00userdata` creates, on the user partition at every boot:
`db  logs  .ndsys  .pycache  .seedrng  sdcard  tones  wallpapers`.
The C port keeps this script; `.pycache` becomes dead but removing it is a behaviour
change and is out of scope for the 1:1 port.

---

### B. Prop-file parsing — **three different dialects, do not unify them**

They differ in ways the tests actually exercise. Implement three functions in
`nd_props.c` and pick the right one per call site.

#### B-1. `nd_props_parse_settings()` — used for `settings.prop` and `version.prop`
Python: `SettingsStorage._parse_settings` (`SettingsStorage/__init__.py:27`).

```
read the WHOLE file as text, decoded strict UTF-8
  -> any decode error, or any OSError, aborts and yields an EMPTY map
split on line boundaries (Python str.splitlines(): \n, \r, \r\n all count)
for each line:
    line = strip leading/trailing ASCII+Unicode whitespace
    if line is empty                      -> skip
    if line starts with '#'               -> skip
    if line contains no '='               -> skip
    key, value = split on the FIRST '='
    map[strip(key)] = strip(value)        -- last duplicate wins
```

**Critical**: the strict-UTF-8 requirement is observable. `test_settings_version_layering.py:87`
writes `b"\x00\xff not a prop file"` into `version.prop` and requires the whole file to
read as empty. A C implementation that just splits bytes must therefore **validate UTF-8
over the whole file first** and return an empty map on failure. (`load_settings()` prints
`[Settings] Failed to read <path>: <exc>` on failure; `load_version()` is silent.)

#### B-2. `nd_props_parse_lenient()` — used for `/run/neodct/sdcard.prop`
Python: `Storage._read_state` (`Storage/__init__.py:31`).

Same as B-1 **except**:
- decoding uses `errors="replace"` — invalid bytes become U+FFFD instead of aborting, so
  a corrupt file still yields whatever lines parsed. (`test_storage.py:164` writes
  `b"\x00\xffgarbage"` and expects state `absent`, which it gets because no line has an
  `=`, not because the file was rejected.)
- the split is `partition("=")`, i.e. first `=`, identical result.

#### B-3. `nd_props_parse_raw_lines()` — used for `state.prop` and `relay.conf`
Python: `RemoteShell._read_props` (`RemoteShell/__init__.py:92`).

```
open in text mode, iterate LINES (no whole-file read)
for each line (INCLUDING its trailing newline, NOT pre-stripped):
    if '=' not in line                 -> skip
    if line starts with '#'            -> skip      # unstripped: a leading space defeats this
    key, value = split on FIRST '='
    map[strip(key)] = strip(value)
only OSError is caught (-> empty map). A UTF-8 decode error PROPAGATES to the caller.
```

The "leading space defeats the comment check" behaviour is a real difference from B-1/B-2
and must be reproduced. The decode error propagating out of `settings()` is caught only
by the launcher's blanket `except Exception` at `launcher.py:104`.

#### B-4. `nd_props_write_atomic()` — the writer
Two writers exist and they differ in exactly one edge case.

`SettingsStorage._format_settings` (`:40`):
```
lines = for each key in ASCII-sorted order:  "key=value"
text  = "\n".join(lines) + "\n"
```
So an **empty** settings map produces a file containing a single `"\n"` byte.

`RemoteShell._write_props` (`:105`):
```
for each key in sorted order:  write "key=value\n"
```
An empty map produces a **zero-byte** file.

Both then do:
```
mkdir -p dirname(path)                     (RemoteShell: unconditional; Settings: only if dirname non-empty)
open(path + ".tmp", "w")                   default mode 0644 & ~umask
write(data); flush(); fsync(fd); close()
rename(path + ".tmp", path)                POSIX rename, atomic
```
Note: **neither writer fsyncs the parent directory.** Only `CrashHandler` does that. Do
not add it.

Sorting is Python's `sorted()` on `str`, i.e. code-point order. Every key in the project
is ASCII, so `strcmp()` matches — but sort with `strcmp`, not `strcoll`, so a locale can
never change the file's byte layout.

---

### C. `SettingsStorage` semantics

Precedence, lowest to highest: **`DEFAULTS` < `settings.prop` < `version.prop`**.

```c
/* SettingsStorage.DEFAULTS -- SettingsStorage/__init__.py:15 */
"system.audio.ringtone"      = "/NeoDCT/System/tones/Low.mp3"
"system.ui.wallpaper"        = "NONE"
"system.ui.engineering_mode" = "ON"
"system.os.versionnumber"    = "0.3.1a"
"system.os.versionname"      = "NeoDCT System v0.3.1a"
"system.os.platform"         = "unknown"
"system.hw.battery_i2c_bus"  = "3"
"system.hw.battery_i2c_addr" = "0x36"
```

`SYSTEM_PREFIX = "system.os."` — anything under it describes the image, never a user
preference, and is **never written to `settings.prop`**.

```
load_settings()   -> parse SETTINGS_PATH with B-1. Missing file -> {}. Error -> {} + log line.
load_version()    -> parse VERSION_PATH  with B-1. Any error    -> {} silently.

save_settings(s)  -> s2 = { k:v in s | not startswith(k, "system.os.") }
                     write s2 atomically (B-4 Settings dialect)
                     ALL exceptions swallowed; on failure print
                     "[Settings] Failed to write <path>: <exc>"

load_with_defaults(defaults):
    stored   = load_settings()
    result   = copy(defaults); result.update(stored); result.update(load_version())
    stale    = [k in stored where k startswith "system.os."]
    missing  = [k in defaults where k not in stored]
    if stale or missing or not exists(SETTINGS_PATH):
        save_settings(result)
    return result

get_setting(key, dflt) -> load_with_defaults(DEFAULTS).get(key, dflt)
set_setting(key, val)  -> s = load_with_defaults(DEFAULTS); s[key] = val; save_settings(s)
```

#### W-1 — the write-on-every-read quirk. **Measured, not theoretical.**

`DEFAULTS` contains three `system.os.*` keys. `save_settings()` strips exactly those
keys before writing. Therefore they are **never** present in `stored`, therefore
`missing` is **permanently non-empty**, therefore `load_with_defaults()` writes the file
on **every single call**.

Verified against the real module: five consecutive `get_setting()` calls produced five
full atomic rewrites (temp file + `fsync` + `rename`).

Consequences the C port inherits if it is a strict 1:1:
- Every `get_setting()` on the phone = 1 file read of `settings.prop`, 1 file read of
  `version.prop`, 1 create + write + `fsync` + `rename`.
- `NotifyService`, `BatteryService` and `ModemService` all call `get_setting()` from hot
  paths. `ModemService._pcm_port()` is called per call setup; `NotifyService._ringtone()`
  per ring.
- On UBIFS/NAND this is real flash wear.

**This is logged in `OPEN-QUESTIONS.md`.** Until answered, implement it exactly, but put
the write behind one function (`nd_settings_flush_if_needed()`) so switching to
"write only when the content actually changed" is a one-line change.

#### C-1. Every settings key in the project

Grepped across the whole overlay. This is the complete set.

| Key | In `DEFAULTS` | Default at call site | Read by | Written by |
| --- | --- | --- | --- | --- |
| `system.audio.ringtone` | `/NeoDCT/System/tones/Low.mp3` | `""` | `NotifyService/__init__.py:128` (`RING_SETTING`) | `apps/Tones/main.py:207` |
| `system.ui.wallpaper` | `NONE` | `"NONE"` | `core/main.py:623` | `apps/Settings/main.py:89` |
| `system.ui.engineering_mode` | `ON` | `"ON"` | `core/main.py:636`, `apps/Settings/main.py:160`, `apps/Update/main.py:85` | `apps/Settings/main.py:171` (`"ON"`/`"OFF"`) |
| `system.os.versionnumber` | `0.3.1a` | `""` | `launcher.py:46`, `engineering/apps/Downgrade/main.py:70`, `apps/Settings/main.py:185`, `apps/Update/main.py:90` | never (image fact) |
| `system.os.versionname` | `NeoDCT System v0.3.1a` | `"NeoDCT OS"` | `apps/Settings/main.py:184` | never |
| `system.os.platform` | `unknown` | `"unknown"` | `engineering/apps/Downgrade/main.py:76`, `apps/Update/main.py:281,373` | never |
| `system.os.buildtime` | — | `"Unknown"` | `apps/Settings/main.py:186` | never (only in `version.prop`) |
| `system.os.buildepoch` | — | — | `ClockService/__init__.py:78` — read **directly** from `version.prop`, not via `get_setting` | never |
| `system.hw.battery_i2c_bus` | `3` | `3` (int) | `BatteryService/__init__.py:93` | never in-app |
| `system.hw.battery_i2c_addr` | `0x36` | `"0x36"` | `BatteryService/__init__.py:94`, parsed `int(x, 0)` | never in-app |
| `system.hw.modem_at_port` | — | `"AUTO"` (`DEFAULT_PORT`) | `ModemService/__init__.py:153` | never |
| `system.hw.modem_pcm_rate` | — | `"16000"` | `ModemService/__init__.py:162` | never |
| `system.hw.modem_pcm_port` | — | `"AUTO"` | `ModemService/__init__.py:632` | never |
| `system.hw.modem_mic_device` | — | `"AUTO"` | `ModemService/__init__.py:652` | never |
| `system.modem.allow_calls` | — | `"ON"` | `ModemService/__init__.py:173` | never |
| `system.ui.wpeverywhere` | — | `"ON"` | `lib/nd_ui.c` `chrome_settings_load()` | never in-app |
| `system.ui.wpeverywhere_dim` | — | `"0.75"` | `lib/nd_ui.c` `chrome_settings_load()` | never in-app |
| `calllog.duration.last` | — | `"0"` | `apps/CallLog/main.py:95` | `apps/CallLog/main.py:103` |
| `calllog.duration.received` | — | `"0"` | same | same |
| `calllog.duration.dialed` | — | `"0"` | same | same |
| `games.snake.level` | — | `5` | `apps/Games/main.py:68,72` | `apps/Games/main.py:75` |
| `games.snake.topscore` | — | `0` | `apps/Games/main.py:54,77` | `apps/Games/main.py:56` |
| `games.memory.topscore` | — | `0` | `apps/Games/main.py:54,91` | `apps/Games/main.py:56` |

The last two have no Python ancestor: they arrived with the C build, when the
framework started drawing the wallpaper behind its own chrome. Both are read
with a call-site default and are deliberately **not** in `DEFAULTS` — a key in
that table is written into `settings.prop` on the first read (the R-24 quirk
above), and these two are consulted from inside the render path, which would
turn every repaint into a flash write. Neither has a screen in the Settings
app; they are taste, and the wallpaper picker is already where a person goes
to change how the phone looks.

`system.ui.wpeverywhere_dim` is a SECOND brightness multiplier applied on top
of the 0.3 the home screen already uses, so the default 0.75 puts chrome at
`0.3 * 0.75` of the original picture. It is parsed with `strtod` and anything
unparseable, negative or above 1 falls back to the default rather than being
clamped to an extreme nobody can then explain.

Boolean parsing is **not** uniform. Two distinct rules exist:

- `apps/Settings/main.py:122` `_setting_is_enabled(value, default=True)` — lowercase the
  stripped string; `("1","true","on","yes","enabled")` → true;
  `("0","false","off","no","disabled")` → false; **anything else returns `default`**.
  Used for `system.ui.engineering_mode` in `core/main.py:635` and `apps/Settings`.
- `ModemService/__init__.py:174` — `val.strip().upper() in ("ON","1","TRUE","YES")`.
  Anything else is false. Used for `system.modem.allow_calls`.
- `apps/Update/main.py:85` — a third form:
  `str(get_setting("system.ui.engineering_mode","ON")).strip().upper()` compared against
  a literal (read the Update spec for the exact comparison).

Port all three separately. Do not introduce one shared `nd_setting_bool()`.

#### C-2. `version.prop` — the format the build writes

Generated by `neodct/scripts/post-build-system-metadata.sh:65`:

```
# Generated by post-build-system-metadata.sh -- do not edit.
# Facts about this image. User settings are in /NeoDCT/User/settings.prop.
system.os.versionnumber=<VERSION_ID from /etc/os-release>
system.os.versionname=NeoDCT System v<VERSION_ID>
system.os.platform=<last argv, or "unknown" if empty or containing '/'>
system.os.buildtime=<UTC "%Y-%m-%d %H:%M UTC">
system.os.buildepoch=<SOURCE_DATE_EPOCH or date -u +%s>
```

This file is **not** in the overlay — it is created at build time and lives in the
read-only squashfs. The C port does not write it.

---

### D. `Storage` — SD-card state

```c
#define ND_SD_MOUNT_POINT "/NeoDCT/User/sdcard"
#define ND_SD_STATE_FILE  "/run/neodct/sdcard.prop"
static const char *ND_SD_FOLDERS[5] =
    { "wallpapers", "tones", "backup_db", "music", "update" };   /* ORDER MATTERS for setup */
#define ND_SD_UPDATE_SUFFIX   ".ndsw"
#define ND_SD_PREFERRED_UPDATE "UPDATE.ndsw"
```

`Card` is a 6-field record: `state, device, fstype, label, mountpoint, removable`.

`card()` (`Storage/__init__.py:47`):
```
values   = parse STATE_FILE with dialect B-2
reported = values["state"]  or ""
fstype   = values["fstype"] or ""
device   = values["device"] or ""
removable = (fstype != "virtiofs")           # note: computed BEFORE any early return

if reported in {"unmountable", "unformatted"}:
    return Card("unformatted", device, fstype, values["label"] or "", MOUNT_POINT, removable)
if reported not in {"mounted", "share", "ready"}:
    return Card("absent", "", "", "", MOUNT_POINT, removable)     # device/fstype/label BLANKED
return Card(_has_folders() ? "ready" : "needs_setup",
            device, fstype, values["label"] or "", MOUNT_POINT, removable)
```

`_has_folders()` — `os.path.isdir(MOUNT_POINT "/" name)` for **all five** folders.
`is_ready()` — `card().state == "ready"`.
`folder(name)` — `NULL` unless `is_ready()`, else `MOUNT_POINT "/" name`. Note it does
**not** check that `name` is one of the five.
`setup_folders()` — `mkdir -p` all five under `MOUNT_POINT`; **false** on the first
`OSError`, true otherwise. (Folders already created before the failure stay created.)
`media_dirs(kind, system_dir)` — build `[system_dir?, folder(kind)?]`, then keep only
entries that are existing directories, in that order. Stock content always first.
`find_updates()`:
```
dir = folder("update");  if NULL -> []
names = listdir(dir);    on OSError -> []
keep names where lowercase(name) endswith ".ndsw" AND isfile(dir/name)
sort ascending by raw name                       (first pass)
then stable-sort by key (name != "UPDATE.ndsw", lowercase(name))   (second pass)
return absolute paths
```
The two-pass sort matters: the second sort is stable, so files with equal keys keep the
first sort's order. In C: single sort with comparator
`(a=="UPDATE.ndsw" ? 0:1) <=> (b=="UPDATE.ndsw" ? 0:1)`, tie-break `strcasecmp`,
tie-break `strcmp`.

`removable` is only consumed by the Settings app and by `test_storage.py:74`, which
asserts a `virtiofs` share reports `removable == False`.

---

### E. sqlite schemas

`init_databases()` runs from `NeoDCT_UI.__init__` (`core/main.py:1114`), before anything
else. It is a plain function at module level, not a method (`core/main.py:441`).

```
db_path = "/NeoDCT/User/db"
if not exists(db_path):  print("[CORE] Creating User DB directory: <path>"); makedirs(db_path)
```

**phonebook.db**
```sql
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS contacts
    (id INTEGER PRIMARY KEY AUTOINCREMENT,
     name TEXT,
     number TEXT,
     speed_dial INTEGER);
```
then:
```sql
SELECT count(*) FROM contacts;
-- if 0:  print "[CORE] Seeding default contacts..."
INSERT INTO contacts (name, number, speed_dial) VALUES ('NeoDCT Support', '555-1234', 2);
COMMIT;
```

**sms_inbox.db**
```sql
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS inbox
    (id INTEGER PRIMARY KEY AUTOINCREMENT,
     message TEXT,
     sender TEXT,
     timestamp INTEGER,
     is_read INTEGER DEFAULT 0);
```

**sms_outbox.db**
```sql
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS outbox
    (id INTEGER PRIMARY KEY AUTOINCREMENT,
     message TEXT,
     timestamp INTEGER);
```

Then `print("[CORE] Databases initialized successfully.")`.

**call_log.db** is *not* created by `init_databases()`. It is created lazily by
`apps/CallLog/main.py:47` `_connect()`, which `makedirs` the parent and then:
```sql
CREATE TABLE IF NOT EXISTS calls
    (id INTEGER PRIMARY KEY AUTOINCREMENT,
     type TEXT,             -- 'missed' | 'received' | 'dialed'
     number TEXT,
     timestamp INTEGER,
     duration INTEGER DEFAULT 0);
```
No `PRAGMA journal_mode=WAL` on `call_log.db`. Reproduce that asymmetry.

The comment in `init_databases` claims WAL is inherited by later connections
("automagically!!! (hopefully)"). It is: `journal_mode=WAL` is persisted in the database
header, so every later `sqlite3_open()` picks it up. `call_log.db` therefore stays in
rollback-journal mode forever. Do not add the pragma.

#### E-1. Every SQL statement the shipped code issues

`core/main.py:1039` `_store_incoming_sms(sender, body)` — **re-creates the table
defensively** before inserting:
```sql
CREATE TABLE IF NOT EXISTS inbox (...same as above...);
INSERT INTO inbox (message, sender, timestamp, is_read) VALUES (?, ?, ?, 0);   -- (body, sender, int(time()))
```
returns `cursor.lastrowid`, then `print("[NOTIFY] SMS stored (id <id>) from <sender>")`.

`core/main.py:1123` `_count_unread_sms()`:
```sql
SELECT COUNT(*) FROM inbox WHERE is_read = 0;
```
any exception → `0`.

`apps/Messages/main.py`:
```sql
SELECT id, message, sender, timestamp, is_read FROM inbox ORDER BY timestamp DESC;
SELECT id, message, timestamp FROM outbox ORDER BY timestamp DESC;
SELECT id, message, sender, timestamp, is_read FROM inbox WHERE id = ?;
UPDATE inbox SET is_read = 1 WHERE id = ?;
DELETE FROM inbox  WHERE id = ?;
DELETE FROM outbox WHERE id = ?;
CREATE TABLE IF NOT EXISTS outbox (...);           -- in _save_outbox_message
INSERT INTO outbox (message, timestamp) VALUES (?, ?);
```
Each of those first checks `os.path.exists(<db>)` and returns `[]` / `None` if absent —
so a missing database is not an error, it is an empty inbox.

`apps/PhoneBook/shared/list_ui.py:23`:
```sql
SELECT * FROM contacts WHERE name LIKE ? ORDER BY name ASC;   -- '%' + query + '%'
SELECT * FROM contacts ORDER BY name ASC;
```
Rows are consumed positionally as `(id, name, number, speed_dial)`, so the column order
in `SELECT *` is load-bearing — keep the `CREATE TABLE` column order.

`apps/PhoneBook/main.py`:
```sql
INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, 0);
UPDATE contacts SET name=?, number=? WHERE id=?;
DELETE FROM contacts WHERE id=?;
```

`apps/CallLog/main.py`:
```sql
SELECT number, timestamp FROM calls WHERE type=? ORDER BY id DESC LIMIT 20;
DELETE FROM calls;                  -- "All"
DELETE FROM calls WHERE type=?;     -- missed | dialed | received
```

Engineering seed tools (`engineering/tools/debug_sms_seed_inbox.py:47`,
`debug_phonebook_createrandomcontacts.py:113`) declare byte-identical schemas. Whether
those tools get ported at all is an app-team decision; the schemas must not drift.

#### E-2. The WAL / backup hazard

`apps/Update/main.py:166` `_backup_user_data()` copies `*.db` from `/NeoDCT/User/db` to
`<card>/backup_db/<%Y%m%d-%H%M%S>/` with `shutil.copy2`. It does **not** copy the
`-wal` and `-shm` sidecars. With `journal_mode=WAL` a `.db` copied without its WAL is
missing every commit since the last checkpoint. This is an existing bug. Per the
"port the bug too" rule it is reproduced; it is listed in Risks and in
`OPEN-QUESTIONS.md`.

---

### F. `CrashHandler`

```c
#define ND_CRASH_IMAGE_PATH   "/NeoDCT/System/ui/resources/CRASH.jpg"
#define ND_CRASH_DEFAULT_NOTICE "An application has crashed."
/* CONTINUE_KEYS = {14, 28, 46, 50, 96} -- BACKSPACE, ENTER, C, M, KP_ENTER */
static const int ND_CRASH_CONTINUE_KEYS[5] = { 14, 28, 46, 50, 96 };
#define ND_CRASH_LOG_DIR      "/NeoDCT/User/logs"
#define ND_CRASH_LOG_PATH     "/NeoDCT/User/logs/crash.log"
#define ND_CRASH_LOG_ROTATED  "/NeoDCT/User/logs/crash.log.1"
#define ND_CRASH_LOG_MAX_BYTES (64 * 1024)      /* 65536; cap is 2x this in total */
```

#### F-1. `is_simulation()`
`return !access("/dev/ttyFIQ0", F_OK)` inverted — i.e. **true when `/dev/ttyFIQ0` does
not exist**. Any error → true. Mirrors `launcher.py:15`.

#### F-2. The log record — byte-exact template

`log_crash(source, exc_info, note)` builds:

```
============================================================
time:   YYYY-MM-DD HH:MM:SS (epoch 1234567890)
mode:   QEMU/simulation            <- or "hardware"
source: <source>
uptime: <uptime>   mem: <mem>
note:   <note>                     <- this line only when note is non-empty
<traceback text, trailing whitespace stripped>
```

and appends `"\n"`. Precisely:

- Line 1 is exactly **60** `=` characters.
- `time:` uses three spaces after the colon (`"time:   "`), `mode:` three, `source:` one
  (`"source: "`), `uptime:` one (`"uptime: "`), `note:` three (`"note:   "`).
  Written out: `"time:   "`, `"mode:   "`, `"source: "`, `"uptime: "`, `"note:   "`.
- Timestamp: `time.strftime("%Y-%m-%d %H:%M:%S")` — **local time**, then
  `(epoch <int(time.time())>)`.
- `mode:` is the literal `QEMU/simulation` or `hardware`.
- Between uptime and mem there are **three** spaces: `f"uptime: {u}   mem: {m}"`.
- `_uptime()`: read `/proc/uptime`, take the first whitespace-separated field, append
  `"s"` → e.g. `"1234.56s"`. Any error → `"?"`.
- `_mem_available()`: read `/proc/meminfo` line by line; the first line starting with
  `"MemAvailable"` **or** `"MemFree"` is collapsed with `" ".join(line.split())` and
  returned. **`/proc/meminfo` lists `MemFree` before `MemAvailable`, so in practice this
  always returns the `MemFree` line** — e.g. `"MemFree: 41200 kB"`. Reproduce the quirk:
  scan lines in order, first prefix match on either name wins. Any error → `"?"`.
- If there is no exception info, the last line is the literal
  `(no exception info available)`.
- Whole record = `"\n".join(lines) + "\n"`.

Write sequence:
```
makedirs(CRASH_LOG_DIR, exist_ok=True)
if exists(CRASH_LOG_PATH) and size(CRASH_LOG_PATH) > 65536:  rename -> CRASH_LOG_ROTATED
open(CRASH_LOG_PATH, "a", encoding="utf-8"); write(report); flush(); fsync(fd); close()
for d in ("/NeoDCT/User/logs", "/NeoDCT/User"):
    fd = open(d, O_RDONLY); fsync(fd); close(fd)      # each wrapped in try/except OSError
```
The parent-directory fsyncs matter: without them a power pull right after a crash loses
the newly created file's directory entry. Keep both, in that order.

If `is_simulation()`, also print to the (already colour-wrapped) stdout:
```
[CRASH] <source>: <summary or "(no exception info)"> (report -> /NeoDCT/User/logs/crash.log)
```

`log_crash()` returns the log path on success, `NULL` on any failure. **It must never
propagate an error.** The whole body is inside one `try/except Exception: return None`.

#### F-3. `_exc_summary` — the one-line summary
```
if no exception info: NULL
text = "<ExceptionTypeName>: <str(exception)>"
if len(text) <= 90: return text
else:               return text[:87] + "..."
```
Length is in **characters**, not bytes. Every producer in the C port emits ASCII, so byte
truncation at 87 is equivalent — but if a message can carry UTF-8 (an app name, a file
path), truncate on a character boundary.

**In C the summary is synthesised from the signal**, since there is no exception. Proposed
form, which keeps the same shape and the same 90/87 cap:
```
"SIGSEGV: <AppName> died at 0x0001a2b4"
"SIGABRT: <AppName> aborted"
"exit 3: <AppName> exited non-zero"
```
This is a **deviation in content, not in layout**. Flagged in `OPEN-QUESTIONS.md`.

#### F-4. `_flush_input(ui)`
```
fd = ui.keypad_fd; if NULL -> return
loop:  select([fd], timeout=0.0); if not readable -> break
       read(fd, 24)  -- 24 bytes, one struct input_event on this build; any error -> break
```
The literal `24` is in the source (`CrashHandler/__init__.py:166`, and identically in
`framework.py` `MessageDialog._flush_input`). Keep it; do not substitute
`sizeof(struct input_event)` without confirming the target's `time_t` width, since a
mismatch changes how many events get drained.

#### F-5. The engineering crash screen — exact pixels

Screen constants (`core/main.py:39`): `UI_WIDTH = 240`, `UI_HEIGHT = 175`,
`SOFTKEY_HEIGHT = 30`, so `content_bottom = 145`.
`_draw_engineering_crash_screen(ui, summary)` reads them via `getattr` with those same
fallbacks, so the numbers are 240 / 175 / 30 / 145 in every case.

```
1.  fill the whole canvas black.
      Python: draw.rectangle((0, 0, 240, 175), fill="black")
      PIL rectangles are INCLUSIVE on both ends -> x 0..240, y 0..175, clipped to the
      240x175 canvas. In C: memset the frame to black.

2.  try: img = Image.open("/NeoDCT/System/ui/resources/CRASH.jpg").convert("RGB")
    on ANY failure -> img = None

3.  if img:
        img = img.resize((240, 175), LANCZOS)
        canvas.paste(img, (0, 0))

    CRASH.jpg is ALREADY 240x175 (verified). Pillow's Image.resize() short-circuits when
    the target size equals the source size and the box is the whole image -- it returns
    a plain copy with NO resampling. So in C this is a straight JPEG decode and blit;
    no LANCZOS kernel is needed for the shipped asset. Only implement resampling here if
    a future CRASH.jpg is a different size.

4.  else (fallback, image missing/unreadable):
        text = "CRASH"
        font = ui.font_xl (24px)  or ui.font_n (20px)  or ui.font_s (14px), first present
        if font:
            w, h = get_text_size("CRASH", font)      # textbbox((0,0), ...) w/h
            x = (240 - w) // 2
            y = max(0, (145 - h) // 2)               # note: content_bottom, not screen_h
            draw.text((x, y), "CRASH", font=font, fill="white")

5.  summary strip (only if summary is non-empty AND ui.font_s exists):
        _, th = get_text_size(summary, font_s)       # font_s == 14px
        draw.rectangle((0, 0, 240, th + 4), fill="black")   # INCLUSIVE -> th+5 rows, 0..th+4
        draw.text((2, 2), summary, font=font_s, fill="white")
    The whole strip is inside a try/except that swallows failures.
    The summary is NOT wrapped and NOT ellipsised beyond the 90-char cap in F-3, so a
    long summary runs off the right edge and is clipped by the canvas. Reproduce that.

6.  SoftKeyBar(ui).update("Continue", present=False)
        is_transparent = not hasattr(ui, "softkey").  The core UI object assigns
        self.softkey at core/main.py:596, BEFORE any crash can happen, so this bar is
        ALWAYS the opaque variant here.
        -> draw.rectangle((0, 145, 240, 175), fill="black")      # covers the bottom of CRASH.jpg
        -> w, h = get_text_size("Continue", font_n)              # font_n == 20px
           x = (240 - w) // 2
           y = 145 + ((30 - h) // 2)
           draw.text((x, y), "Continue", font=font_n, fill="white")

7.  ui.fb.update(ui.canvas)      -- one present, at the end
```

Then `_wait_for_continue(ui)`: loop `ui.wait_for_key()`, return the first key in
`{14, 28, 46, 50, 96}`. **Every other key is ignored** — including the power key.

Font sizes come from `core/main.py:606`: `font_s = 14`, `font_md = 18`, `font_n = 20`,
`font_xl = 24`, all from `/NeoDCT/System/ui/resources/fonts/font.ttf`.

#### F-6. `show_app_crash(ui, message, app_name, exc_info)`

```
exc_info  defaults to the exception currently being handled
log_crash(app_name or "app", exc_info)          <- ALWAYS, before any drawing
try:
    summary = _exc_summary(exc_info)
    if _is_engineering_mode(ui, default=False):     # getattr(ui,"engineering_mode",None);
                                                    # None -> the default (False here)
        _flush_input(ui)
        _draw_engineering_crash_screen(ui, summary)
        _wait_for_continue(ui)
        return
    MessageDialog(ui, summary ? f"{message}\n{summary}" : message).show()
except Exception:
    return          # a crash in the crash handler is silently discarded
```
Note the default for `_is_engineering_mode` is **False** here (so a `ui` object with no
`engineering_mode` attribute gets the plain dialog), while `apps/Settings/main.py:160`
uses `default=True`. Do not unify.

The non-engineering dialog uses `MessageDialog`'s own defaults: `title=None`,
`icon_path=None` → `DEFAULT_WARNING_ICON` (the 24×24 warning.png), `button_text="OK"`,
`accept_keys=(28,)`, `cancel_keys=(14,)`, `margin=8`.

#### F-7. Where crashes are raised from today, and what replaces them in C

| Python site | Today | C equivalent |
| --- | --- | --- |
| `core/main.py:930` `launch_app()` `except BaseException` | prints `[OS] App crashed: <name> (<path>)`, `traceback.print_exc()`, then `show_app_crash(ui, app_name=…, exc_info=…)`; `finally: gc.collect(); self._unread_sms = self._count_unread_sms()` | `waitpid()` on the `nd-apprun` child. `WIFSIGNALED` → crash. `WIFEXITED && status != 0` → crash. `WIFEXITED && status == 0` → clean return. Always re-read the unread-SMS count afterwards. |
| `core/main.py:948` `render_menu()` `except BaseException` | prints `[OS] Menu crashed`, traceback, `log_crash("menu", …)`; `finally: self.state = "HOME"`. **No crash screen.** | explicit error returns in the menu code, funnelled to `nd_crash_log("menu", …)`; state forced back to `HOME`. |
| `core/main.py:1312` main loop `except BaseException` | prints `[CORE] Unhandled exception in main loop`, traceback, `log_crash("core-main-loop", …)`, `sleep(0.1)`. **No crash screen.** | error returns from `nd_core_tick()`; log and sleep 100 ms. A genuine `SIGSEGV` in the core is not survivable and falls through to the shell handler below. |
| `core/main.py:1146` `_open_notification()` `except BaseException` | prints `[NOTIFY] Read flow crashed`, traceback. **No `log_crash`, no screen.** | same: log to serial only. |
| `bin/run_neodct.sh:42` | UI process exited: red `\033[41m\033[1;97m` on `/dev/tty0`, `clear`, three `=`-rules and `CODE: $EXIT_CODE` | **unchanged shell script**; the `python3 /NeoDCT/launcher.py` line becomes the C core binary |
| `apps/Crash` engineering app (`engineering/apps/Crash/main.py`, 17 lines) | deliberately raises, to exercise the path | port as an app that deliberately dereferences NULL, so the whole `waitpid` path is testable on-device |

`IncomingCall` and `KeyboardInterrupt` are re-raised, never treated as crashes
(`core/main.py:925`). In C the equivalent is: a child exiting with a reserved status
code meaning "the phone rang" / "the user cancelled" is not a crash.

#### F-8. The crash-log truncation bug — **must be flagged, must be reproduced**

`bin/run_neodct.sh:29` runs the UI as:
```sh
python3 /NeoDCT/launcher.py 2> "$CRASH_LOG"      # CRASH_LOG=/NeoDCT/User/logs/crash.log
```
`2>` opens with `O_TRUNC`. So the carefully `fsync`ed crash history from the previous
boot is **wiped at the start of every boot**. Inside the process, `launcher.py:24`
replaces `sys.stderr` with the serial device, so Python-level stderr no longer goes
there — but fd 2 still points at the truncated file, and any C library or subprocess
writing to raw fd 2 interleaves with `CrashHandler`'s appends at conflicting offsets.

Also note the fallback at `run_neodct.sh:26`: if `mkdir -p /NeoDCT/User/logs` fails the
script uses `/tmp/crash.log` instead — but `CrashHandler` always uses
`/NeoDCT/User/logs/crash.log` regardless, so the two diverge on a read-only user
partition.

Reproduce as-is; entry filed in `OPEN-QUESTIONS.md`.

---

### G. `ErrorScreen`

```c
#define ND_ERR_WARNING_ICON "/NeoDCT/System/ui/resources/img/errorscreen/warning.png"
#define ND_ERR_ACK_FILE     "/NeoDCT/User/.ack_security_warning"
```

`show_error(ui, message, title=NULL, icon_path=WARNING_ICON, button_text="OK",
accept_keys={28}, cancel_keys={14}, wait_for_ack=true)`:
- builds a `MessageDialog` with exactly those arguments
- `wait_for_ack == false` → `dlg.render()` (flush input, draw, present) and return
  "no key"; the caller owns what happens next
- otherwise → `dlg.show()` and return the dismissing key code

`show_alpha_security_notice_once(ui, ack_path=ND_ERR_ACK_FILE, message=NULL)`:
```
if message is NULL:
    message = "This is alpha software. Consider it extremely insecure and unstable. "
              "Don't store important data on this device."
              -- ONE line in the source; the wrap comes from MessageDialog
if exists(ack_path):  return false          # exception here is swallowed and falls through
show_error(ui, message, title="Notice", icon_path=WARNING_ICON, button_text="OK")
best effort:
    makedirs(dirname(ack_path), exist_ok=True)
    write str(int(time.time())) to ack_path, encoding utf-8, mode "w"    # NO trailing newline
return true
```
The ack file content is the decimal epoch seconds as ASCII with **no newline**. Nothing
reads it back; only its existence matters.

Called once, from `NeoDCT_UI.__init__` at `core/main.py:618` — after the fonts are
loaded and after `self.softkey` exists, but **before** the wallpaper is loaded. So the
notice is always drawn on a black background with an opaque softkey bar.

Other `show_error` call sites (`core/main.py`):
- `:1163` low-battery shutdown —
  `show_error(self, "Battery empty. Shutting down...", title="LOW BATTERY", button_text=None, wait_for_ack=False)`.
  `button_text=None` means the softkey bar is drawn black with **no** label.
  Then `sleep(3)`, `os.sync()`, `os.system("poweroff")`; if `poweroff` returns non-zero,
  print `[BATT] poweroff failed (rc=<rc>); resuming so dev sessions survive.` and return;
  otherwise loop forever on `sleep(1)`.
- `:1189` battery warning — `show_error(self, message, title="Battery")` where message is
  `"BATTERY CRITICALLY LOW!"` or `"LOW BATTERY!"`.

#### G-1. `MessageDialog` layout — needed because the crash/error screens depend on it

Owned by the UI-framework spec (`ui/framework.py:990`), reproduced here because these
screens are only pixel-correct if it is. With `margin = 8`:

```
clear (0,0,240,175) black
icon = ui.get_image(icon_path)          -> RGBA, cached; NULL on failure
if icon: canvas.paste(icon, (8, 8), icon)          # alpha-composited with itself as mask
y = 8
if title and font_title (font_md, 18px):
    title_x = 8 + (icon ? icon.width + 6 : 0)      # 8 + 30 = 38 with the 24x24 icon
    draw.text((title_x, 8), title, font=font_md, fill="white")
    _, th = get_text_size(title, font_md)
    y = max(y, 8 + th + 6)
if icon:
    y = max(y, 8 + icon.height + 6)                # 8 + 24 + 6 = 38
max_w = 240 - 16 = 224
alert_font  = font_n (20px)
alert_lines = wrap(message, alert_font, 224)
if len(alert_lines) <= 2:  font_body, lines, centered = font_n, alert_lines, True
else:                      font_body, lines, centered = font_s, wrap(message, font_s, 224), False
line_h    = get_text_size("Ag", font_body).h + 3
max_lines = max(1, int((145 - y - 8) / line_h))
if len(lines) > max_lines:
    lines = lines[:max_lines]
    lines[-1] += " …"    (U+2026 HORIZONTAL ELLIPSIS, preceded by a space) unless it already ends with …
y += max(0, (145 - 8 - y - len(lines)*line_h) // 2)
for each line:
    x = centered ? max(8, (240 - line_w)//2) : 8
    draw.text((x, y), line, font=font_body, fill="white")
    y += line_h
SoftKeyBar(ui).update(button_text, present=False)
ui.fb.update(ui.canvas)
```
`show()` = `_flush_input()`, `_draw()`, then loop `wait_for_key()` until the key is in
`accept_keys` or `cancel_keys`, and return it.
`render()` = `_flush_input()`, `_draw()`, return nothing.

---

### H. `RemoteShell`

#### H-1. Constants
```c
#define ND_RS_CARD_DIR             "remote"        /* folder on the SD card */
#define ND_RS_CARD_CONF            "relay.conf"
#define ND_RS_SSHD                 "/usr/sbin/sshd"
#define ND_RS_SSH                  "/usr/bin/ssh"
#define ND_RS_KEYGEN               "/usr/bin/ssh-keygen"
#define ND_RS_SFTP_SERVER          "/usr/libexec/sftp-server"
#define ND_RS_LOCAL_PORT           22
#define ND_RS_DEFAULT_RELAY_PORT   2222
#define ND_RS_DEFAULT_RELAY_USER   "neodct"
#define ND_RS_RETRY_SECONDS        15
```

**`LOCAL_PORT = 22` even though the comment above it says "Not 22: … a number nobody
guesses costs nothing either."** The comment is stale; the value is 22. Port the value,
keep the comment as-is with a note, do not "fix" either.

`CARD_FILES` — an ordered mapping, iterated in insertion order:
```
"id_ed25519"      -> /NeoDCT/User/.remote/relay_id_ed25519
"authorized_keys" -> /NeoDCT/User/.remote/authorized_keys
"known_hosts"     -> /NeoDCT/User/.remote/known_hosts
```

#### H-2. `settings()` / `save_settings()`
`settings()` parses `STATE_FILE` with dialect **B-3** and returns:
```
enabled : values["enabled"] == "1"                   (missing -> "0" -> false)
host    : values["host"]  or ""
user    : values["user"]  or "neodct"
port    : values["port"]  or "2222"                  -- a STRING, never an int
```

`save_settings(host, user, port, enabled)` — each argument optional; only non-NULL ones
change anything:
```
current = settings()
if host    != NULL:  current.host = strip(host)                              # empty allowed
if user    != NULL:  current.user = strip(user) or "neodct"
if port    != NULL:  current.port = strip(str(port)) or "2222"
if enabled != NULL:  current.enabled = bool(enabled)
write STATE_FILE atomically (dialect B-4, RemoteShell variant) with exactly four keys:
    enabled = "1" or "0"
    host, user, port
return current
```
Sorted order on disk is therefore always: `enabled`, `host`, `port`, `user`.

#### H-3. `install_keys_from_card(card_root)`
```
source = card_root + "/remote"
if not isdir(source):  raise RemoteShellError('No "remote" folder on the card.')
makedirs(USER_DIR, exist_ok=True); chmod(USER_DIR, 0700)
taken = []
for (name, destination) in CARD_FILES in insertion order:
    path = source + "/" + name
    if not isfile(path): continue
    data = read whole file as bytes
    fd = open(destination, O_WRONLY|O_CREAT|O_TRUNC, 0600)     # mode set AT CREATION,
    write(fd, data); close(fd)                                 # not chmod'ed afterwards
    chmod(destination, 0600)                                   # belt and braces, for pre-existing files
    taken.append(name)
conf = parse(source + "/relay.conf", dialect B-3)
if conf non-empty:
    save_settings(host=conf.get("host"), user=conf.get("user"), port=conf.get("port"))
    taken.append("relay.conf")
if taken is empty: raise RemoteShellError('No keys in remote/ on the card.')
return taken
```
The `O_CREAT` with mode 0600 rather than create-then-chmod is deliberate — the comment at
`RemoteShell/__init__.py:171` explains that on slow flash the window between the two is
long enough to matter. Reproduce it exactly: `open(..., O_WRONLY|O_CREAT|O_TRUNC, 0600)`.
Note the process umask can still clear bits from the creation mode, which is why the
explicit `chmod` follows. Keep both.

Error message strings, verbatim (they are shown on the phone):
- `No "remote" folder on the card.` — built as `'No "%s" folder on the card.' % CARD_DIR`
- `No keys in remote/ on the card.` — built as `'No keys in %s/ on the card.' % CARD_DIR`

#### H-4. Keys
`have_keys()` — `isfile(RELAY_KEY) && isfile(AUTHORIZED_KEYS)`. (Currently unused by any
caller; port it anyway.)

`ensure_host_key()`:
```
if isfile(HOST_KEY): return HOST_KEY
makedirs(USER_DIR, exist_ok=True); chmod(USER_DIR, 0700)
run: /usr/bin/ssh-keygen -q -t ed25519 -N "" -f /NeoDCT/User/.remote/ssh_host_ed25519_key
     stdout and stderr both to /dev/null; return code ignored
if not isfile(HOST_KEY): raise RemoteShellError("Could not make a host key.")
chmod(HOST_KEY, 0600)
return HOST_KEY
```
The `-N ""` is an empty-string argument, not an omitted one.

`host_fingerprint()`:
```
if not isfile(HOST_KEY + ".pub"): return ""
out = check_output([KEYGEN, "-lf", HOST_KEY + ".pub"], stderr=DEVNULL)
return out.decode("ascii", "replace").strip()
on OSError or non-zero exit -> ""
```

#### H-5. `write_sshd_config()` — the generated file, byte for byte

Regenerated on **every** start, never read back, mode 0600, ASCII-encoded, written with
`open(SSHD_CONFIG, O_WRONLY|O_CREAT|O_TRUNC, 0600)` — **not** via the atomic-rename
writer.

```
# Generated by System/core/RemoteShell. Edits are overwritten.
ListenAddress 127.0.0.1
Port 22
HostKey /NeoDCT/User/.remote/ssh_host_ed25519_key
AuthorizedKeysFile /NeoDCT/User/.remote/authorized_keys
PermitRootLogin prohibit-password
PasswordAuthentication no
KbdInteractiveAuthentication no
ChallengeResponseAuthentication no
PermitEmptyPasswords no
StrictModes no
PubkeyAuthentication yes
X11Forwarding no
AllowAgentForwarding no
AllowTcpForwarding no
PidFile /NeoDCT/User/.remote/sshd.pid
Subsystem sftp /usr/libexec/sftp-server
```
(The lines are joined with `"\n"` and the tuple ends with an empty string, so the file
ends with exactly one `\n`.)

Things the tests assert and that must not drift
(`test_remoteshell.py:44,57,71,81,88,100,277,488`):
- `ListenAddress 127.0.0.1` present; `0.0.0.0` absent; `ListenAddress ::` absent
- all four "no password" directives present, plus `PermitRootLogin prohibit-password`
  and `PubkeyAuthentication yes`
- `AllowTcpForwarding no`, `AllowAgentForwarding no`, `X11Forwarding no`
- `Subsystem sftp` present
- **`UsePAM` absent** — this openssh has no PAM and logs `Unsupported option UsePAM`
- `StrictModes no` present
- rewriting it discards manual edits
- file mode has no group/other bits (`mode & 0o077 == 0`)

`sshd_command()` → `["/usr/sbin/sshd", "-f", SSHD_CONFIG, "-D", "-e"]`.

#### H-6. `tunnel_command(host, user, port)` — argv, exactly
```
/usr/bin/ssh
-N
-T
-i  /NeoDCT/User/.remote/relay_id_ed25519
-o  IdentitiesOnly=yes
-o  BatchMode=yes
-o  StrictHostKeyChecking=yes
-o  UserKnownHostsFile=/NeoDCT/User/.remote/known_hosts
-o  ExitOnForwardFailure=yes
-o  ServerAliveInterval=30
-o  ServerAliveCountMax=3
-o  ConnectTimeout=20
-R  <port>:127.0.0.1:22
<user>@<host>
```
`port` is interpolated as a string (`"%s:127.0.0.1:%d" % (port, LOCAL_PORT)`), so a port
value of `"02222"` stays `"02222"`. The last argument is `"%s@%s" % (user, host)` with no
quoting of any kind at this level.

#### H-7. `write_tunnel_script(host, user, port)` — the generated shell script

Mode **0700**, `open(TUNNEL_SCRIPT, O_WRONLY|O_CREAT|O_TRUNC, 0700)`, ASCII.

```sh
#!/bin/sh
# Generated by System/core/RemoteShell. Edits are overwritten.
while :; do
    echo "[RSHELL] dialling <user>@<host>"
    '/usr/bin/ssh' '-N' '-T' '-i' '/NeoDCT/User/.remote/relay_id_ed25519' '-o' 'IdentitiesOnly=yes' '-o' 'BatchMode=yes' '-o' 'StrictHostKeyChecking=yes' '-o' 'UserKnownHostsFile=/NeoDCT/User/.remote/known_hosts' '-o' 'ExitOnForwardFailure=yes' '-o' 'ServerAliveInterval=30' '-o' 'ServerAliveCountMax=3' '-o' 'ConnectTimeout=20' '-R' '<port>:127.0.0.1:22' '<user>@<host>'
    echo "[RSHELL] connection ended ($?); retrying in 15"
    sleep 15
done
```
- The ssh line is `" ".join(_quote(part) for part in tunnel_command(...))` and is indented
  with **four spaces**.
- `_quote(w)` = `"'" + str(w).replace("'", "'\\''") + "'"` — single-quote everything, and
  render an embedded `'` as `'\''`. This is the shell-injection defence and
  `test_remoteshell.py:145` runs a real `sh` against it with a hostile host of
  `a'; touch <marker>; '` and asserts the marker is not created and the final argv is
  `neodct@a'; touch <marker>; '`.
- The `echo "[RSHELL] dialling %s@%s"` line uses the **unquoted** user and host —
  interpolated into a double-quoted shell string, so a `"` or `$` in the host would break
  out of *that* echo. It is a log line only; reproduce as-is and note it.
- `($?)` must be literal in the file — `test_remoteshell.py:286` asserts `"%s" not in
  script` and `"($?)" in script`, because an earlier version used `echo` with a `%s`
  placeholder that got printed literally.
- `sleep 15` must be present (`test_remoteshell.py:297`).
- File ends with a trailing `\n` (the joined tuple ends with `""`).

#### H-8. pid handling — the bug this shape exists for

`_pid_from(path)` — read the file, `int(strip(contents))`; `OSError` or `ValueError` → NULL.

`_owns(pid, needle)`:
```
if pid falsy (NULL or 0): return false
read /proc/<pid>/cmdline as bytes; OSError -> false
decode utf-8 with "replace"
return needle is a SUBSTRING of the decoded string
```
`/proc/<pid>/cmdline` is NUL-separated; a plain substring search over the whole buffer is
what Python does and is what C must do (search the raw bytes for the needle, ignoring the
NULs). Needles used: `"sshd"` and `"tunnel.sh"`.

`_stop_pid(path, needle)`:
```
pid = _pid_from(path)
if _owns(pid, needle):
    try:    killpg(getpgid(pid), SIGTERM)
    except: try: kill(pid, SIGTERM)  except: pass
unlink(path)    # ALWAYS, even when the pid was not ours; OSError ignored
```

The comment at `RemoteShell/__init__.py:371` records the incident this guards against: a
phone left `sshd.pid=244` and `tunnel.pid=246` behind, rebooted, and `stop()` SIGTERM'd
the process group those numbers had been handed to — its own launcher. The UI never
started and the serial log stopped mid-boot with no error, because a signal is not an
exception. **The `/proc` check is not optional.**

`status()`:
```
{ "sshd":    _owns(_pid_from(SSHD_PID),   "sshd"),
  "tunnel":  _owns(_pid_from(TUNNEL_PID), "tunnel.sh"),
  "enabled": settings()["enabled"] }
```

#### H-9. `check_ready()` — refusal order and exact messages
```
if not isfile("/usr/sbin/sshd"):     raise "This build has no ssh server."
current = settings()
if not current["host"]:              raise "No relay address set."
if not isfile(RELAY_KEY):            raise "No relay key. Copy one from the card."
if not isfile(AUTHORIZED_KEYS):      raise "No authorized_keys. Copy one from the card."
if not isfile(KNOWN_HOSTS):          raise "No known_hosts for the relay. Copy one from the card."
return current
```
The order is tested (`test_remoteshell.py:182,187,197`). Each string is shown verbatim in
a `MessageDialog` on the phone.

#### H-10. `start()` — ordering matters
```
current = check_ready()

# loosen a world/group-writable /NeoDCT/User, best effort
try:
    parent = dirname(USER_DIR)                       # "/NeoDCT/User"
    if isdir(parent) and (stat(parent).st_mode & 0o022):
        chmod(parent, 0o755)
except OSError: pass

ensure_host_key()
write_sshd_config()
write_tunnel_script(current.host, current.user, current.port)

log = open(LOG_FILE, "ab", buffering=0)     # append, binary, UNBUFFERED. Opened BEFORE
                                            # anything can fail, so a failed start still
                                            # leaves a log (test_remoteshell.py:433).
stop(remember=False)                        # never end up with two of either

try:
    Popen(sshd_command(), stdout=log, stderr=log, start_new_session=True)
    for _ in range(20):                     # up to 2.0 s in 0.1 s steps
        if _owns(_pid_from(SSHD_PID), "sshd"): break
        sleep(0.1)
    tunnel = Popen(["/bin/sh", TUNNEL_SCRIPT], stdout=log, stderr=log,
                   start_new_session=True)
    write "%d\n" % tunnel.pid  to TUNNEL_PID
except OSError as exc:
    raise RemoteShellError("Could not start: %s" % exc)

save_settings(enabled=True)
return status()
```
`start_new_session=True` is `setsid()` in the child — it is what makes
`killpg(getpgid(pid), SIGTERM)` in `stop()` take down the `ssh` that `tunnel.sh` is
waiting on, rather than orphaning it.

In C: `fork()` then **immediately** `setsid()` and `execve()` (per
`CODING-STANDARDS.md` §1.1 — the core has modem/clock threads, so nothing between fork
and exec may allocate). `setsid()` is async-signal-safe and is allowed there. Redirect
the child's fd 1 and 2 to the log fd with `dup2()` before the exec. `_exit(127)` if
`execve` fails.

Note: `sshd` writes its own PidFile (from the generated config); the tunnel's pid is
written by the parent. The `log` file handle is never closed in the Python — it leaks an
fd per `start()`. In C, close the parent's copy after the second `fork` (the children keep
their `dup2`'d copies); this changes nothing observable and avoids the leak. **This is the
only place this spec permits a divergence from the Python, and it is behaviour-neutral.**

`stop(remember=True)`:
```
_stop_pid(TUNNEL_PID, "tunnel.sh")      # tunnel first, then sshd
_stop_pid(SSHD_PID,   "sshd")
if remember: save_settings(enabled=False)
return status()
```

`start_if_enabled()` — called from `launcher.py:103`, before the framebuffer is opened:
```
if not settings()["enabled"]: return NULL
try: return start()
except RemoteShellError as exc:
    print("[RSHELL] not starting: %s" % exc)
    return NULL
```
It runs before the network is up on purpose: `tunnel.sh` is a retry loop and mobile data
can take a minute to attach, or never attach.

#### H-11. The engineering app (`engineering/apps/RemoteShell/main.py`)

```
APP_ID = 9990,  TITLE = "Remote",  KEY_ENTER = 28
manifest.json: {"name": "Remote Shell", "id": "9990", "icon": "icon.png", "exec": "main.py"}
```
`TITLE` is `"Remote"` not `"Remote Shell"` for a measured reason (comment at line 24):
"Remote Shell" renders 189 px against 136 px available beside the breadcrumb counter;
"Remote" is 111 px.

Menu, rebuilt on every loop because it doubles as the status display:
```
index 0 STATUS      "Status: On" | "Status: Dialling" | "Status: Off"
                      On       = sshd AND tunnel
                      Dialling = sshd XOR tunnel      (relay refused/dropped the tunnel)
                      Off      = neither
index 1 TOGGLE      "Turn off" if (sshd or tunnel) else "Turn on"
index 2 RELAY       "Relay: <host>"   -- or "Relay: not set" when host is empty
index 3 LOGIN       "Login: <user>"
index 4 PORT        "Port: <port>"
index 5 KEYS        "Copy keys from card"
index 6 FINGERPRINT "This phone's key"
```
Rendered with `VerticalList(ui, "Remote", lines, app_id=9990)` and
`SoftKeyBar(ui).update("Select", present=False)`. `choice < 0` returns from the app.
Selecting index 0 (STATUS) does nothing.

Actions:
- TOGGLE, currently running → `RemoteShell.stop()` then dialog
  `"Remote Shell is off."`
- TOGGLE, currently stopped → confirm dialog
  `"Let this phone be reached over the internet?"` with button `"Turn on"`; only if the
  dialog returns key 28 → `RemoteShell.start()`, then
  `"Remote Shell is on.\n\nIt stays on across restarts until you turn it off here."`
  A `RemoteShellError` is caught and its message shown instead.
- RELAY → `TextInput(ui, "Remote", "Relay host:", initial_text=host)`, no input filter
  (so the punctuation cycle is available for `:` in IPv6 literals). `None` → cancel.
- LOGIN → `TextInput(..., "Login:", initial_text=user, input_filter="letters")`
- PORT → `TextInput(..., "Relay port:", initial_text=port, input_filter="numbers")`
- KEYS → if `not isdir(Storage.MOUNT_POINT)` → `"No card in the phone."`; else
  `install_keys_from_card(Storage.MOUNT_POINT)` and on success
  `"Copied: <comma-joined sorted(taken)>.\n\nDelete them from the card now -- anyone who takes the card out can read them."`
  (note the literal double hyphen, not an em dash)
- FINGERPRINT → `ensure_host_key()`, then
  `"This phone:\n<fingerprint or 'unknown'>"`

All dialogs are `MessageDialog(ui, message, title="Remote", button_text="OK")`.

---

### I. `logstyle` — serial log colouring

```c
#define ND_LOG_RESET "\033[0m"
#define ND_LOG_BOLD  "\033[1m"
/* 256-colour foreground: "\033[38;5;%dm" */
```

Named palette (`logstyle.py:31`) — must match `etc/neodct-colors.sh`:

| Tag | Code | | Tag | Code |
| --- | --- | --- | --- | --- |
| `MODEM` | 39 | | `SETUP` | 214 |
| `ndsys` | 33 | | `UI` | 120 |
| `UPDATE` | 33 | | `FB` | 123 |
| `CORE` | 46 | | `KERNEL` | 244 |
| `OS` | 46 | | `sdcard` | 180 |
| `Launcher` | 82 | | `CLOCK` | 129 |
| `BATT` | 226 | | `RSHELL` | 162 |
| `FUEL` | 226 | | `Browser` | 141 |
| `NOTIFY` | 201 | | `CRASH` | 196 |
| `INPUT` | 51 | | `ERROR` | 196 |
| `KEYMAP` | 87 | | `FATAL` | 196 |

`APP_TAGS = ("Koki", "Music", "CallLog", "Settings", "PB", "Tones", "Games", "Messages",
"Clock", "Calculator", "Power")`.

`ERROR_COLOUR = 196`.

`_colour_for(tag)`:
```
if tag in TAG_COLOURS:  return TAG_COLOURS[tag]
if tag in APP_TAGS:     return 141 + (sum of ord(c) for c in tag) % 36
                        # 141..176, a purple/pink band, stable per app name
return 22 + (sum of ord(c) for c in tag) % 180
                        # 22..201, avoiding the darkest greys and the reserved reds
```
`ord(c)` is the Unicode code point. Every tag in the project is ASCII, so summing
`unsigned char` bytes is equivalent — but if a non-ASCII tag ever appears, C must sum
code points, not bytes, to get the same colour.

`colour_enabled()`:
```
if getenv("NO_COLOR") is set (to ANY value, including "")  -> false
return getenv("NEODCT_COLOR", default "1") not in {"0", "no", "off"}
```

`paint(text, code, bold)` → `(bold ? BOLD : "") + "\033[38;5;<code>m" + text + "\033[0m"`,
or `text` unchanged when colouring is off.

`_split_tag(line)`:
```
if line does not start with '[':      return (NULL, line)
end = index of the first ']'
if end < 2:                           return (NULL, line)      # "[]" and "[x" are not tags
tag = line[1:end]
if tag is empty, or any char is not (alnum or '_' or '-'):  return (NULL, line)
return (tag, line[end+1:])            # the rest INCLUDES the character right after ']'
```
`isalnum()` here is Python's Unicode-aware version. ASCII-only in practice.

`_Painter` — a line-oriented stream wrapper:
- `write(data)` accumulates into a pending buffer, and for every complete `"\n"`-terminated
  line writes `render(line) + "\n"` to the underlying stream, then flushes. Returns
  `len(data)`.
- The buffering exists because `print()` writes the text and the newline as two separate
  calls, and a tag split across two writes would not be recognised.
- `render(line)`: if colouring is off, or the line is blank after stripping → unchanged.
  If a forced colour is set (stderr) → paint the whole line in that colour. Otherwise
  split the tag; no tag → unchanged; tag → `paint("[" + tag + "]", colour, bold=True) + rest`.
- `flush()` emits any partial pending line **unpainted through `render`** (so a process
  dying mid-line still gets that line out) and clears the buffer.
- `isatty()` / `fileno()` / everything else delegate to the wrapped stream.

`install(stdout=True, stderr=True)`:
```
_ENABLED = colour_enabled()
if sys.stdout already has the _neodct_painted marker: return _ENABLED   (idempotent)
wrap sys.stdout with a _Painter (no forced colour)
wrap sys.stderr with a _Painter forced to ERROR_COLOUR (196)
```
Everything on stderr is painted red because tracebacks arrive there as untagged lines.

`rule(char="=", width=72, code=46)` → `paint(char * width, code, bold=True)`.
`banner_lines(path="/etc/neodct-banner")` → the file's contents with trailing `\n`s
stripped, split on `\n`; `[]` on `OSError`. **Note the name collision**:
`NotifyService.banner_lines()` (`core/main.py:891`) is a completely different function
about the on-screen notification banner. Give them different C names
(`nd_log_banner_lines()` vs `nd_notify_banner_lines()`).

Only two callers exist: `launcher.py:31` (`logstyle.install()` — done *after* the stdio
redirect to serial, never before) and `apps/Browser/main.py:78` (`logstyle.paint`).
`rule()` and `banner_lines()` have no Python caller; the shell side does the boot banner
(`etc/init.d/S99banner`). Port them anyway — they are 6 lines and the shell script's
fallbacks reference the same widths.

In C the natural shape is not a stream wrapper but a `printf`-style logger:
`nd_log(ND_LOG_CORE, fmt, ...)` emits `"\033[1m\033[38;5;46m[CORE]\033[0m " + text + "\n"`.
Keep a raw `nd_log_line(const char *line)` that runs the tag-splitting path, so ported
code that already builds `"[TAG] …"` strings keeps working unchanged.

---

## Public interface (the functions other parts call)

```c
/* ---- nd_props.h : shared prop-file plumbing (libneodct) ---- */
typedef struct nd_props nd_props;                 /* ordered key/value map, ASCII-sorted on write */

nd_props *nd_props_new(void);                     /* owned by caller; free with nd_props_free */
void      nd_props_free(nd_props *p);
const char *nd_props_get(const nd_props *p, const char *key, const char *dflt);
nd_err    nd_props_set(nd_props *p, const char *key, const char *value);   /* copies both */
bool      nd_props_has(const nd_props *p, const char *key);
size_t    nd_props_count(const nd_props *p);
const char *nd_props_key_at(const nd_props *p, size_t i);  /* sorted iteration */

/* the three dialects -- see Behaviour B. All return an EMPTY map, never NULL,
 * except nd_props_parse_raw which reports a decode error to the caller. */
nd_props *nd_props_parse_settings(const char *path);   /* B-1: strict UTF-8, whole file */
nd_props *nd_props_parse_lenient(const char *path);    /* B-2: errors="replace"        */
nd_err    nd_props_parse_raw(const char *path, nd_props **out);  /* B-3: may report ND_ERR_IO */

nd_err nd_props_write_atomic(const char *path, const nd_props *p, bool trailing_nl_when_empty);
                                        /* true  -> SettingsStorage._format_settings
                                         * false -> RemoteShell._write_props        */

/* ---- nd_settings.h (libneodct) ---- */
nd_err      nd_settings_init(void);                  /* loads the DEFAULTS table */
const char *nd_settings_get(const char *key, const char *dflt);   /* == get_setting  */
nd_err      nd_settings_set(const char *key, const char *value);  /* == set_setting  */
nd_props   *nd_settings_effective(void);   /* == load_with_defaults(DEFAULTS); caller frees */
/* Path overrides, for the host test harness only -- mirrors monkeypatching
 * SettingsStorage.SETTINGS_PATH / VERSION_PATH in test_settings_version_layering.py */
void        nd_settings_set_paths(const char *settings_path, const char *version_path);

/* ---- nd_storage.h (libneodct) ---- */
typedef enum { ND_CARD_ABSENT, ND_CARD_READY, ND_CARD_NEEDS_SETUP, ND_CARD_UNFORMATTED } nd_card_state;
typedef struct {
    nd_card_state state;
    char device[64];        /* "/dev/vdc"  */
    char fstype[32];        /* "vfat"      */
    char label[64];
    char mountpoint[128];   /* always ND_SD_MOUNT_POINT */
    bool removable;         /* fstype != "virtiofs" */
} nd_card;

void   nd_storage_card(nd_card *out);                       /* never fails */
bool   nd_storage_is_ready(void);
bool   nd_storage_folder(const char *name, char *out, size_t n);  /* false unless ready */
bool   nd_storage_setup_folders(void);
size_t nd_storage_media_dirs(const char *kind, const char *system_dir,
                             char out[][256], size_t max);   /* existing dirs, stock first */
size_t nd_storage_find_updates(char out[][256], size_t max); /* UPDATE.ndsw first */
void   nd_storage_set_paths(const char *mount_point, const char *state_file);  /* tests */

/* ---- nd_db.h (libneodct) ---- */
nd_err nd_db_init_all(void);            /* == init_databases(): dirs, schemas, seed contact */
nd_err nd_db_open(const char *path, sqlite3 **out);   /* plain sqlite3_open_v2 wrapper */
int64_t nd_db_store_incoming_sms(const char *sender, const char *body);  /* rowid, or -1 */
int     nd_db_count_unread_sms(void);   /* 0 on any error */
/* Per-table accessors live with their apps; only the schema strings are owned here. */
extern const char *const ND_SCHEMA_CONTACTS;
extern const char *const ND_SCHEMA_INBOX;
extern const char *const ND_SCHEMA_OUTBOX;
extern const char *const ND_SCHEMA_CALLS;

/* ---- nd_crash.h (core) ---- */
bool        nd_crash_is_simulation(void);
const char *nd_crash_log(const char *source, const nd_crash_info *info, const char *note);
            /* returns ND_CRASH_LOG_PATH or NULL; NEVER fails into the caller */
void        nd_crash_show_app(nd_ui *ui, const char *message /* or NULL for the default */,
                              const char *app_name, const nd_crash_info *info);

/* what waitpid() gives us, standing in for sys.exc_info() */
typedef struct {
    bool     from_signal;
    int      signo;         /* WTERMSIG  */
    int      exit_status;   /* WEXITSTATUS */
    void    *fault_addr;    /* si_addr, when known; else NULL */
    char     detail[512];   /* the child's own backtrace, read from its crash-report fd */
} nd_crash_info;

/* ---- nd_errscreen.h (libneodct) ---- */
int  nd_errscreen_show(nd_ui *ui, const char *message, const char *title,
                       const char *icon_path, const char *button_text,
                       const int *accept_keys, size_t n_accept,
                       const int *cancel_keys, size_t n_cancel,
                       bool wait_for_ack);          /* returns the dismissing key, or -1 */
bool nd_errscreen_alpha_notice_once(nd_ui *ui, const char *ack_path, const char *message);

/* ---- nd_remoteshell.h (core) ---- */
typedef struct { bool enabled; char host[256]; char user[64]; char port[16]; } nd_rs_settings;
typedef struct { bool sshd; bool tunnel; bool enabled; } nd_rs_status;

void   nd_rs_settings_get(nd_rs_settings *out);
nd_err nd_rs_settings_save(const char *host, const char *user, const char *port,
                           const bool *enabled);            /* NULL = leave unchanged */
nd_err nd_rs_install_keys_from_card(const char *card_root, char *taken, size_t n,
                                    char *errmsg, size_t errn);
bool   nd_rs_have_keys(void);
nd_err nd_rs_ensure_host_key(char *errmsg, size_t n);
nd_err nd_rs_host_fingerprint(char *out, size_t n);         /* "" when unavailable */
nd_err nd_rs_write_sshd_config(void);
nd_err nd_rs_write_tunnel_script(const char *host, const char *user, const char *port);
nd_err nd_rs_check_ready(nd_rs_settings *out, char *errmsg, size_t n);
nd_err nd_rs_start(nd_rs_status *out, char *errmsg, size_t n);
nd_err nd_rs_stop(nd_rs_status *out, bool remember);
void   nd_rs_status_get(nd_rs_status *out);
void   nd_rs_start_if_enabled(void);                        /* boot path; logs and swallows */

/* ---- nd_log.h (libneodct) ---- */
bool nd_log_colour_enabled(void);
int  nd_log_colour_for(const char *tag);
void nd_log_paint(char *out, size_t n, const char *text, int code, bool bold);
void nd_log(const char *tag, const char *fmt, ...);         /* "[TAG] …\n", painted */
void nd_log_err(const char *tag, const char *fmt, ...);     /* whole line in 196 */
void nd_log_line(const char *line);                         /* tag-split an already-built line */
void nd_log_rule(char *out, size_t n, char ch, int width, int code);
size_t nd_log_banner_lines(const char *path, char out[][128], size_t max);
```

Every `errmsg` buffer carries the exact Python exception string, because those strings are
displayed to the user on the phone.

---

## External dependencies and their C replacements

| Dependency | Used for | C replacement |
| --- | --- | --- |
| `sqlite3` (Python module) | contacts, SMS inbox/outbox, call log | **libsqlite3 directly.** Already in the Buildroot config (`sqlite3` is listed in `AGENTS.md` as available). Use `sqlite3_open_v2` + `sqlite3_prepare_v2` + bound parameters — never string-concatenated SQL. `sqlite3_exec` is fine for the `CREATE TABLE IF NOT EXISTS` and `PRAGMA` statements. Link against the same libsqlite3 the Python module wraps, so the on-disk format is byte-identical and existing user databases survive the upgrade. |
| `PIL.Image.open` (`CRASH.jpg`) | the engineering crash screen background | in-house rasterizer + libjpeg-turbo (already in the image for Pillow). The asset is exactly 240×175, so decode-and-blit; no resampling. |
| `PIL.Image.resize(LANCZOS)` | nominally resizes CRASH.jpg to the screen | **not needed for the shipped asset** — Pillow short-circuits when source size == target size. Keep a stub that asserts the sizes match, so a future differently-sized CRASH.jpg fails loudly instead of silently drawing wrong. |
| `ui.canvas.paste`, `ui.draw.rectangle`, `ui.draw.text`, `ui.get_text_size` | crash screen, message dialogs | the shared rasterizer + FreeType text (UI-framework spec). `get_text_size` must be the same `textbbox((0,0))` width/height, i.e. `(bbox.right-bbox.left, bbox.bottom-bbox.top)`. |
| `PIL.Image.open` (`warning.png`, RGBA) + alpha `paste` | the dialog icon | PNG decode (libpng, already present) + alpha compositing in the rasterizer. |
| `subprocess.call([KEYGEN, …])` | make the phone's host key | `fork()` + `setsid()`-free direct `execve("/usr/bin/ssh-keygen", …)`, `waitpid()`. stdout/stderr to `/dev/null` via `dup2`. |
| `subprocess.check_output([KEYGEN, "-lf", …])` | read the fingerprint | `pipe()` + `fork()` + `execve` + read + `waitpid`. Decode as ASCII with replacement for invalid bytes; strip. |
| `subprocess.Popen(..., start_new_session=True)` | launch `sshd` and `tunnel.sh` | `fork()`; in the child, `setsid()`, `dup2(logfd, 1)`, `dup2(logfd, 2)`, `execve()`, `_exit(127)`. **Nothing else between fork and exec** (`CODING-STANDARDS.md` §1.1). |
| `subprocess.call([SDCARD_HELPER, "format", dev])` (Settings app) | reformat a card | same fork/exec/waitpid; the helper stays a shell script. |
| `os.killpg` / `os.getpgid` / `os.kill` | stop the tunnel and sshd | `killpg(getpgrp_of(pid), SIGTERM)` via `getpgid(2)`; fall back to `kill(pid, SIGTERM)`. |
| `os.replace` | atomic settings/state writes and log rotation | `rename(2)` — already atomic within a filesystem, which all of these are. |
| `os.fsync(fd)` on a **directory** fd | make a newly created crash log durable | `open(dir, O_RDONLY)` + `fsync()` + `close()`. Works on ext4 and UBIFS. |
| `os.makedirs(exist_ok=True)` | everywhere | a small `nd_mkdir_p()`; treat `EEXIST` as success. |
| `os.open(path, O_WRONLY\|O_CREAT\|O_TRUNC, 0600/0700)` | keys, sshd_config, tunnel.sh | identical `open(2)` call. The mode-at-creation (rather than chmod-after) is deliberate. |
| `os.chmod` | 0700 on `.remote`, 0755 on `/NeoDCT/User` | `chmod(2)`. |
| `os.stat().st_mode & 0o022` | detect a loose user partition | `stat(2)`. |
| `/proc/<pid>/cmdline`, `/proc/uptime`, `/proc/meminfo` | pid ownership, crash-log context | plain `open`/`read`. All three are small; use a fixed 4 KB stack buffer, no `malloc`. |
| `time.strftime("%Y-%m-%d %H:%M:%S")`, `time.time()`, `time.localtime()` | crash log timestamps, ack file | `time()`, `localtime_r()`, `strftime()`. **Local time**, not UTC. |
| `traceback.format_exception` | the crash log body | no equivalent. Replaced by the `nd_crash_info.detail` buffer, filled by `nd-apprun`'s signal handler using glibc `backtrace()` / `backtrace_symbols_fd()`, or a manual frame-pointer walk if the ARM build is `-fomit-frame-pointer`. **This is the one place the crash log's content genuinely cannot be 1:1.** |
| `sys.stdout` / `sys.stderr` replacement (`logstyle._Painter`) | colouring the serial log | no stream-wrapper equivalent needed. Provide `nd_log()` / `nd_log_err()` that build the painted line and `write(2)` it in one call. `write(2)` of a single line to a serial tty is atomic enough for this purpose; the pending-buffer machinery exists only because `print()` splits writes. |
| `os.environ` (`NO_COLOR`, `NEODCT_COLOR`, `NEODCT_SERIAL_DEVICE`) | colour on/off, serial device override | `getenv()`. Read once at startup and cache. |
| `select.select([fd], [], [], 0.0)` | draining the keypad before a dialog | `poll(2)` with `timeout = 0`, or `select` — either matches. Read 24 bytes per iteration. |
| Python string `sorted()` | settings file key order, update-package order | `qsort` with `strcmp` (never `strcoll`). |
| Python `str.strip()` | prop parsing | `strip` must match Python's default: it removes **Unicode** whitespace. Every value in this project is ASCII, so stripping `" \t\n\r\v\f"` is equivalent; document the assumption. |
| `str.splitlines()` | prop parsing | Python splits on `\n`, `\r`, `\r\n` (and, for `str`, several Unicode line separators). Split on `\n`, `\r\n` and lone `\r` and you cover every file this project actually reads. |
| UTF-8 strict decoding | `settings.prop` / `version.prop` rejection | a ~30-line UTF-8 validator over the whole buffer, run before parsing. Required by `test_settings_version_layering.py:87`. |
| `zip`/`shutil`/`hashlib` | — | not used by this subsystem. |

**Nothing in this subsystem needs a new Buildroot package.** libsqlite3, libjpeg, libpng
and the openssh binaries are already in the image.

### Memory notes for the 8 MB budget

- The settings map is at most ~25 keys × ~80 bytes = under 2 KB. Use a fixed array of
  `{char key[64]; char value[192];}` (256 B × 32 = 8 KB) allocated once at startup rather
  than a hash table with per-entry `malloc`. This removes allocation from the hottest
  path in the system (W-1).
- `nd_props` for `sdcard.prop` has at most 4 keys — use the same fixed structure.
- The crash-log record is built into a single 4 KB stack buffer; the traceback/backtrace
  detail is capped at 512 bytes (`nd_crash_info.detail`). No allocation in the crash path
  — the crash path is exactly where `malloc` is least trustworthy.
- CRASH.jpg decoded to RGB888 is 240 × 175 × 3 = **126,000 bytes**. Decode it **on demand,
  free it immediately after the blit** — do not keep it resident. It is shown at most once
  per app crash. (Contrast with the wallpaper, which is resident by design.)
- warning.png at 24 × 24 RGBA = 2,304 bytes; that one belongs in the shared image cache.
- sqlite is the single biggest dependency here. `sqlite3_open` on a WAL database maps the
  `-shm` file and allocates a page cache. Set
  `sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0)` and a small explicit
  `SQLITE_CONFIG_PAGECACHE` / `PRAGMA cache_size` at startup, and **close every database
  handle as soon as the query is done** — which is exactly what the Python does today
  (`sqlite3.connect(...)` … `conn.close()` around every single operation). Reproducing
  that open/close-per-query pattern is both 1:1 *and* the right memory behaviour here.
- `libsqlite3` should be linked into `libneodct.so` (or dynamically alongside it) so core
  and apps share the same mapped pages.

---

## Proposed C modules

| Module | Contents | Est. LOC |
| --- | --- | --- |
| `nd_paths.h` | every absolute runtime path from section A as a `#define`; nothing else | 60 |
| `nd_props.c` / `.h` | the fixed-size key/value map, the three parse dialects (B-1/B-2/B-3), the UTF-8 validator, the two atomic writers (B-4), `nd_mkdir_p()` | 300 |
| `nd_settings.c` / `.h` | `DEFAULTS` table, `SYSTEM_PREFIX` filtering, the defaults<settings<version layering, `get`/`set`, the W-1 write-on-read behaviour behind one switchable function, test path overrides | 230 |
| `nd_storage.c` / `.h` | `nd_card`, the four states, `FOLDERS`, `is_ready`, `folder`, `setup_folders`, `media_dirs`, `find_updates` with the two-key sort | 220 |
| `nd_db.c` / `.h` | the four schema strings, `nd_db_init_all()` incl. the WAL pragmas and the seed contact, `nd_db_store_incoming_sms`, `nd_db_count_unread_sms`, a prepared-statement helper | 400 |
| `nd_log.c` / `.h` | `TAG_COLOURS`, `APP_TAGS`, `_colour_for` (both hash bands), `colour_enabled`, `paint`, `_split_tag`, `nd_log`/`nd_log_err`/`nd_log_line`, `rule`, `banner_lines` | 260 |
| `nd_errscreen.c` / `.h` | `show_error` wrapper over `nd_msgdialog`, `alpha_notice_once` + the ack file | 130 |
| `nd_crash.c` / `.h` | `is_simulation`, `_uptime`, `_mem_available` (with the MemFree quirk), the record template, rotation, the file+2-directory fsync, the summary formatter, `_flush_input`, the engineering crash screen, `_wait_for_continue`, `show_app_crash` | 340 |
| `apprun_crash.c` | in `nd-apprun`: signal handlers for SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT, write `si_signo`/`si_code`/`si_addr` + a backtrace to the inherited report fd, then re-raise with the default disposition so `waitpid` reports the real signal | 180 |
| `nd_remoteshell.c` / `.h` | settings prop, card key install, host key + fingerprint, the generated `sshd_config` and `tunnel.sh`, `_quote`, pid read/own/stop, `status`, `check_ready`, `start`, `stop`, `start_if_enabled` | 580 |
| `apps/RemoteShell/app_remoteshell.c` | the seven-item menu, the three text inputs, the six dialogs | 230 |
| `tests/unit/test_props.c`, `test_settings.c`, `test_storage.c`, `test_remoteshell.c`, `test_crashlog.c` | host unit tests mirroring the pytest files 1:1 | 700 |
| **Total** | | **≈ 3,630** (≈ 2,930 excluding tests) |

Module placement:

- **`libneodct.so`**: `nd_paths.h`, `nd_props`, `nd_settings`, `nd_storage`, `nd_db`,
  `nd_log`, `nd_errscreen`. Both core and apps need all of these — the Games app writes
  settings, CallLog writes settings and reads `call_log.db`, PhoneBook and Messages read
  the databases, Settings reads the card state.
- **core binary only**: `nd_crash`, `nd_remoteshell`. Neither is ever called from an app.
  (`RemoteShell` is *driven* by an app, but through the app-to-core request channel, not
  by linking — see Risks R-7.)
- **`nd-apprun`**: `apprun_crash.c`.

---

## Risks

| # | Risk | Severity | Mitigation |
| --- | --- | --- | --- |
| R-1 | **W-1: every `get_setting()` rewrites `settings.prop` with an `fsync`.** A 1:1 port carries this onto NAND, where it is real flash wear and a real latency cost in the modem and notify paths. | **high** | Reproduce exactly, but funnel the write through one function. Filed in `OPEN-QUESTIONS.md`; the fix (compare the formatted bytes against what is on disk and skip the write when identical) is a five-line change once approved, and is observationally identical apart from mtime. |
| R-2 | **The crash log is truncated at every boot** by `run_neodct.sh`'s `2> "$CRASH_LOG"`, destroying the record `CrashHandler` `fsync`ed. The one artefact you need after a bad boot is the one the next boot deletes. | **high** | Reproduce (the script is unchanged), but flag prominently. The fix is `2>>` — one character. Filed in `OPEN-QUESTIONS.md`. |
| R-3 | **`nd_crash_info.detail` cannot reproduce a Python traceback.** A C backtrace gives addresses, not file/line/locals. Debuggability genuinely regresses, exactly as `ARCHITECTURE.md` predicts. | **high** | Keep unstripped `.so` files with a separate `.debug` (or at minimum `-funwind-tables` + symbol names) so `backtrace_symbols` produces function names; record the signal, `si_code`, faulting address and the app name; ship an `addr2line` recipe in the docs. |
| R-4 | **Strict-UTF-8 rejection of `version.prop` is easy to miss in C**, because C code naturally treats a file as bytes. Missing it changes the observable result of `test_a_corrupt_version_prop_does_not_break_settings`. | medium | The UTF-8 validator is a named function with its own unit test that feeds it the exact bytes from the pytest (`b"\x00\xff not a prop file"`). |
| R-5 | **The three prop dialects will get "helpfully" merged into one.** The differences (strip-before-`#`, `errors="replace"`, whole-file vs line-at-a-time) are each load-bearing in at least one test. | medium | Three separate functions with three separate unit tests and a comment on each saying which Python function it mirrors and which test pins it. |
| R-6 | **The `_owns()` pid check gets dropped as "defensive".** The bug it prevents killed the launcher's process group on a real phone and left no error anywhere, because a signal is not an exception. | **high** | The comment at `RemoteShell/__init__.py:371` is copied verbatim into the C. `test_remoteshell.py:379,395,406,421` are ported first, before `nd_rs_stop()` is written. |
| R-7 | **`fork()` in the core, which has threads.** `nd_rs_start()` launches two children from the core process, which runs modem and clock threads. Anything between `fork` and `execve` that touches `malloc` deadlocks. | **high** | `CODING-STANDARDS.md` §1.1. Build the full `argv`/`envp` and open the log fd *before* forking; between fork and exec do only `setsid()`, `dup2()`, `execve()`, `_exit()`. Add a `-Wall` review checklist item and a comment at the fork. |
| R-8 | **WAL sidecars are not backed up.** `Update._backup_user_data()` copies `*.db` only; a WAL database copied without its `-wal` loses every commit since the last checkpoint. | medium | Reproduce (1:1) and flag. The correct fix is `sqlite3_backup_*` or a `PRAGMA wal_checkpoint(TRUNCATE)` before the copy. Filed in `OPEN-QUESTIONS.md`. |
| R-9 | **`SELECT * FROM contacts` is consumed positionally** as `(id, name, number, speed_dial)`. Any change to the `CREATE TABLE` column order silently corrupts the phonebook UI. | medium | Keep the column order byte-identical; add a unit test that asserts `PRAGMA table_info(contacts)` returns exactly those four names in that order. |
| R-10 | **`sqlite3_open` per query.** The Python opens and closes a connection around every single operation. Naively "optimising" that to a long-lived handle changes locking behaviour and WAL checkpointing, and holds `-shm` mappings resident. | medium | Reproduce the open/close pattern. It is both 1:1 and better for the memory budget. |
| R-11 | **The crash screen's summary strip is unclipped.** A 90-character summary at 14 px overruns 240 px and is clipped by the canvas edge. A C implementation that wraps or ellipsises "sensibly" fails the golden-frame diff. | low | Golden-frame test with a deliberately long summary. |
| R-12 | **PIL rectangle coordinates are inclusive on both ends.** `rectangle((0,0,240,th+4))` paints `th+5` rows, not `th+4`. Every `draw.rectangle` in this subsystem must be translated as inclusive. | medium | A single `nd_draw_rect_inclusive()` primitive used everywhere, named so nobody reaches for a half-open variant. Verified: `rectangle((0,0,10,4))` fills 5 rows × 11 columns. |
| R-13 | **`LOCAL_PORT = 22` contradicts the comment directly above it** ("Not 22: … a number nobody guesses"). Somebody will "fix" one or the other. | low | Port the value 22, keep the comment, and add `/* the comment above is stale; the value is 22 and is what the relay's -R target must match */`. |
| R-14 | **`/dev/ttyFIQ0` is the only simulation test.** `is_simulation()` and `launcher.py` both key off it. A hardware variant without a FIQ console reports as QEMU and starts printing `[CRASH]` summaries to a console nobody is watching. | low | Keep the single check, documented. It is harmless — a wrong answer only adds a print. |
| R-15 | **`_flush_input` reads 24 bytes per event.** That is `sizeof(struct input_event)` only for 64-bit `time_t` on 32-bit ARM. If the C build disagrees, the drain reads partial events and the dialog either dismisses instantly or hangs on stale input. | medium | Assert `sizeof(struct input_event) == 24` at compile time on the target; if it is 16, the keypad spec owns the change and this drain follows it. Raise as a cross-subsystem question with the keypad agent. |
| R-16 | **Two unrelated `banner_lines()`.** `logstyle.banner_lines()` (the boot art) and `NotifyService.banner_lines()` (the on-screen notification) share a name. | low | `nd_log_banner_lines()` vs `nd_notify_banner_lines()`. |
| R-17 | **The tunnel script's `echo` line interpolates the host unquoted** into a double-quoted shell string; only the `ssh` invocation itself is `_quote`d. A host containing `"` or `$` breaks that one log line (it cannot execute anything, because the `ssh` argv is fully quoted). | low | Reproduce as-is; the security-relevant path is tested by `test_remoteshell.py:145`. Note it in the C comment. |
| R-18 | **`stop()` unlinks the pid file even when the pid was not ours.** Correct (a stale file would be re-examined forever, `test_remoteshell.py:395`) but looks like a bug and invites "fixing". | low | Comment it, and port `test_a_stale_pid_file_is_cleared_anyway` first. |

---

## Tests that cover this

Run with `python3 -m pytest neodct/tests/ -q` from the repo root. `conftest.py` puts
`neodct/overlay/NeoDCT` on `sys.path`, so `System.core.…` imports resolve exactly as they
do on the device.

| File | Tests | Covers | Usable as a port oracle? |
| --- | --- | --- | --- |
| `neodct/tests/test_settings_version_layering.py` (125 lines, 11 tests) | `SettingsStorage` end to end: version.prop supplies the version; version.prop beats a stale settings.prop; user settings survive; `system.os.*` is never persisted; stale `system.os.*` keys are dropped while other user keys survive; falls back to `DEFAULTS` with no version.prop; a corrupt (non-UTF-8) version.prop reads as empty; `system.os.platform` is available for update checks; an unwritable `SETTINGS_PATH` still reads; `launcher.splash_version()` | **Yes, directly.** Every test monkeypatches `SETTINGS_PATH`/`VERSION_PATH` to a `tmp_path`, which is exactly what `nd_settings_set_paths()` exists for. Port all 11 as C unit tests, one for one. |
| `neodct/tests/test_storage.py` (167 lines, 17 tests) | `Storage`: absent / ready / needs_setup / unformatted; a virtiofs share is ready and non-removable; folders only offered when ready; `setup_folders` success and failure; `media_dirs` ordering and filtering; `find_updates` incl. `UPDATE.ndsw` first and non-`.ndsw` ignored; a corrupt state file reads as absent | **Yes, directly.** Monkeypatches `MOUNT_POINT`/`STATE_FILE` → `nd_storage_set_paths()`. Port all 17. |
| `neodct/tests/test_remoteshell.py` (524 lines, 27 tests) | The generated `sshd_config` (loopback only, no passwords, no forwarding, sftp present, rewritten every time, mode 0600, no `UsePAM`, `StrictModes no`); the tunnel argv (host-key checking, batch mode, `ExitOnForwardFailure`, the `-R` target); **shell-injection resistance, by running a real `sh`**; refusal order and messages; off by default; boot does nothing when disabled and logs when the keys are gone; card key install at 0600 and dir at 0700; missing/empty `remote/` folders; the retry message and `sleep`; **stale-pid handling (4 tests)**; a log exists even when start fails; `relay.conf` parsing incl. partial files; `/NeoDCT/User` gets tightened | **Yes, and this is the most valuable file in the subsystem.** Everything is about generated text and process bookkeeping, none of it needs an sshd. The injection test (`:145`) can be ported verbatim — it shells out to `sh` and checks a marker file. Two tests (`:319`, `:352`) inspect a built Buildroot target tree and skip when absent; keep them as build-time checks. |
| `neodct/tests/test_settings_memory_card.py` (152 lines, 9 tests) | Settings › Memory card: the four card states drive the right dialogs; folder setup vs destructive format; accepting setup creates the folders and keeps existing content; declining does nothing; the format prompt says `EVERYTHING ON IT WILL BE ERASED`; the helper is called as `[SDCARD_HELPER, "format", device]`; formatting needs a device name | Partly. It fakes `MessageDialog`/`TextScroller`, so it tests the app's decision tree, not this subsystem's core — but it pins `Storage.card()`'s state values, which is what matters here. |
| `neodct/tests/test_apps_sdcard_sources.py` (136 lines, 11 tests) | `Storage.media_dirs`/`folder` as the media apps use them: stock content first, a half-laid-out card contributes nothing, music needs a card | Yes, for `nd_storage_media_dirs`. |
| `neodct/tests/test_post_build_metadata.py` (157 lines, 9 tests) | `version.prop` generation: the platform is the **last** argv not the second; a path is never a platform; version and name come from `VERSION_ID`; both `buildtime` and `buildepoch` are written; the user mountpoint exists in the read-only image; no `VERSION_ID` is an error; `os-release` agrees with itself and with the changelog | Yes — but it tests a shell script that does not change. Keep running it unmodified. |
| `neodct/tests/test_sdcard_helper.py` (315 lines) | The `neodct-sdcard` shell script itself, sourced with `NEODCT_SDCARD_SOURCE_ONLY=1` | Unchanged; the script is not ported. |
| `neodct/tests/test_uistub.py` (446 lines) | `uistub.py`'s `/NeoDCT` path remapping — patches `builtins.open`, `io.open`, `os.path.exists/isfile/isdir/getsize`, `os.listdir/walk/makedirs/mkdir/remove/unlink/stat/access`, `os.rename/replace`, `shutil.copyfile/copy/copy2`, **`sqlite3.connect`**, `PIL.Image.open`, `PIL.ImageFont.truetype` | Indirectly, and importantly: the presence of `os.replace` and `sqlite3.connect` in that list is the record of which of this subsystem's syscalls the golden-frame harness has to intercept. The C build needs an equivalent root-prefix override (an `ND_ROOT` env var honoured by `nd_paths.h`). |

**Gaps — nothing tests these today:**

- **`CrashHandler` has zero tests.** No test touches `log_crash`, `_exc_summary`,
  `_uptime`, `_mem_available`, rotation, or the crash screen.
- **`ErrorScreen` has zero tests.** Nothing covers `show_error` or the once-only alpha
  notice.
- **`logstyle` has zero tests.** Nothing covers `_split_tag`, `_colour_for` or the
  `_Painter` line buffering.
- `init_databases()` has no test; the schemas are only implicitly covered by the apps.

The C port should **add** unit tests for all four while porting, because they are the
easiest things in the subsystem to get subtly wrong (the 60 `=` characters, the three
spaces in `"uptime: %s   mem: %s"`, the `MemFree`-before-`MemAvailable` quirk, the
`141 + sum%36` colour band). These are pure functions over strings — trivial to test in C
and worth the twenty minutes.

**Golden-frame coverage needed** (capture from the Python build first, per
`ARCHITECTURE.md`):
1. The engineering crash screen with `CRASH.jpg` present and a short summary.
2. The engineering crash screen with a 90-character summary (checks the unclipped strip).
3. The engineering crash screen with `CRASH.jpg` deliberately absent (the `"CRASH"`
   fallback text, `font_xl`, centred on `content_bottom`).
4. The non-engineering `MessageDialog` crash notice, with and without a summary line.
5. The alpha security notice (title `"Notice"`, icon, long body → `font_s` left-aligned).
6. `"LOW BATTERY!"` and `"BATTERY CRITICALLY LOW!"` (short → `font_n`, centred).
7. `"Battery empty. Shutting down..."` with `button_text=None` (blank softkey bar).

---

## How this could be split across agents

The subsystem divides cleanly into **five packages with almost no shared state**. The only
real dependency is that everything sits on `nd_props`, so that goes first and alone.

**Wave 0 — foundation (1 agent, blocks everything else in this subsystem)**
- `nd_paths.h`, `nd_props.c/.h`, `nd_mkdir_p`, the UTF-8 validator, the atomic writers.
- Deliverable: the three parse dialects and both writers, each with a unit test quoting
  the pytest that pins it.
- ~360 LOC. Small, self-contained, and everything downstream is blocked on it, so it
  should be finished and merged before the other four start.

**Wave 1 — four packages in parallel, no shared files**

| Agent | Files | Depends on | Notes |
| --- | --- | --- | --- |
| **A: settings + storage** | `nd_settings.c/.h`, `nd_storage.c/.h` | `nd_props` | The two most heavily tested modules (28 pytest cases between them) and the two with the least ambiguity. Port `test_settings_version_layering.py` and `test_storage.py` first, then make them pass. Owns the W-1 decision. ~450 LOC + 300 test. |
| **B: databases** | `nd_db.c/.h` | `nd_props` (paths only), libsqlite3 | Isolated — nothing else in this subsystem touches sqlite. Needs to agree the row accessors with the **apps agent** (PhoneBook, Messages, CallLog own the queries; this agent owns the schema strings, `init_all`, and the two SMS helpers in `core/main.py`). ~400 LOC. |
| **C: logging + error screens** | `nd_log.c/.h`, `nd_errscreen.c/.h` | `nd_props` (nothing, really), the rasterizer for `nd_errscreen` | `nd_log` is pure string work with no dependencies at all and can start on day one, in parallel with Wave 0. `nd_errscreen` is a thin wrapper and is blocked on the **UI-framework agent** delivering `nd_msgdialog`. Split into two if the UI framework is late. ~390 LOC. |
| **D: remote shell** | `nd_remoteshell.c/.h`, `apps/RemoteShell/app_remoteshell.c` | `nd_props`, `nd_storage` (for `MOUNT_POINT`), the UI framework for the app | The single biggest and most self-contained piece (~810 LOC). 27 pytest cases, all of which are string/process assertions with no hardware. An agent can do the whole of this without touching anything else. Port the tests first — several of them exist because a real phone bricked its own boot. |

**Wave 2 — crash handling (1 agent, must come last)**
- `nd_crash.c/.h` + `apprun_crash.c`.
- Blocked on: the **core-loop agent** (`waitpid` plumbing, `nd_crash_info`), the
  **UI-framework agent** (`nd_msgdialog`, `SoftKeyBar`, `get_text_size`), the
  **rasterizer agent** (JPEG decode, blit, inclusive `rect`), and Wave 1C
  (`nd_errscreen`, `nd_log`).
- This is the module whose *shape* changes most (exceptions → `waitpid`), so it wants the
  most context and the fewest moving parts underneath it. ~520 LOC.

**Cross-agent coordination points — settle these before Wave 1 starts**

1. **`nd_crash_info` struct** — core-loop agent and crash agent must agree the shape and
   who fills it. Proposed definition is in "Public interface" above.
2. **`nd_ui` handle** — crash and errscreen both need `W`, `H`, `SOFTKEY_H`,
   `content_bottom`, `font_s/md/n/xl`, `keypad_fd`, `engineering_mode`, `canvas`, `draw`,
   `fb`, `get_image`, `get_text_size`, `wait_for_key`. That is the UI-framework agent's
   contract; this subsystem only reads from it.
3. **`sizeof(struct input_event)`** — the 24-byte drain (R-15). Keypad agent decides.
4. **Row accessors for the four tables** — DB agent owns the schema, apps agent owns the
   queries. Agree the accessor signatures once.
5. **The app-to-core channel for Remote Shell.** The engineering RemoteShell app runs in
   `nd-apprun`, but `nd_rs_start()` forks children that must outlive it and must be owned
   by the core. Either the app sends a request over the app↔core channel (preferred, and
   matches "core owns long-lived processes"), or `nd_remoteshell` moves into
   `libneodct.so` and the app forks the children itself (simpler, but then a crashing app
   can orphan an sshd). **Filed in `OPEN-QUESTIONS.md`.**
6. **`ND_ROOT` prefix override** for the golden-frame harness, equivalent to `uistub.py`'s
   path remapping. Whoever writes `nd_paths.h` (Wave 0) defines it; everyone honours it.

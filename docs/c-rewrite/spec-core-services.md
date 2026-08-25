# Core services: Modem, Notify, Clock, Battery — C port specification

*Survey of `neodct/overlay/NeoDCT/System/core/{ModemService,NotifyService,ClockService,BatteryService}/__init__.py`
(1084 + 232 + 250 + 302 = **1868 lines of Python**), plus every call site of those
services elsewhere in the overlay.*

Companion documents: `ARCHITECTURE.md` (why processes, why threads),
`CODING-STANDARDS.md` (fork/exec rule, error convention, logging), and
`spec-core-loop.md`, which owns `main.py` — the tick chokepoint, the home-screen
banner pixels and the crash/shutdown screens. Where the two overlap I say so and
give the number rather than assuming the other spec did.

**These services live in the core process and are NULL in an app's.** What an app is
allowed to ask of the modem and the battery across that boundary — four operations, and
why it is only four — is `spec-app-services.md`. Nothing in this document changes for
it: the service channel is a *caller* of the API below, not a second implementation of
it.

---

## What this does (plain English, for a reader who is not a C programmer)

Four small background services keep the phone honest about the outside world.
None of them draws anything themselves; they hand facts to the home screen and
the call screens, which do the drawing.

**ModemService** is the biggest of the four and the only complicated one. The
SIM7600 modem plugs in over USB and shows up as five serial ports. One of them
(`/dev/ttyUSB2`) takes *AT commands* — old-fashioned typed lines like `AT+CSQ`
that the modem answers with `+CSQ: 20,99` and then `OK`. The modem also talks
without being asked: when someone calls you it spits out `RING`, when a text
arrives it spits out `+CMTI: "SM",3`. Those unasked-for lines are called URCs.
So the service is really two things sharing one wire: a question-and-answer
loop, and a listener for surprises.

It does this without any threads today. Every time the UI checks for a
keypress — which happens roughly ten times a second on every screen — it also
gives the modem a nudge. On that nudge the service reads whatever the modem
has said since last time, files it (a `RING` becomes "the phone is ringing"),
and once every five seconds asks `AT+CSQ` so the little signal bars on the home
screen are current. Registration and carrier name are asked for far less often
(every 20 and 60 seconds) because they hardly change.

It also places and receives calls for real. Dialling sends `ATD5551234;`, then
tells the modem to pipe the call audio out of a *sixth* USB port as raw sound,
and then starts two ordinary command-line programs — `aplay` to push the far
end's voice into the speaker, and `arecord` to push the microphone the other
way. There is no audio code in the service at all; it just starts two programs
and kills them when the call ends.

Text messages work in both directions. Sending is fiddly because the modem
answers `AT+CMGS="555..."` with a bare `>` character and *no newline*, then
waits for the message text followed by a Ctrl-Z. Receiving is push-based: the
modem shouts `+CMTI` the moment a message lands, the service reads it out of
the SIM and then **deletes it from the SIM** — the card only holds about thirty,
and a full card silently stops accepting new ones.

If there is no modem at all (QEMU, or the cable is out) the service says
"Simulation Mode" on the boot log and keeps working, driven by four files you
can write to from a serial console: write a number into `/tmp/neodct_sim_csq`
and the bars move; write into `/tmp/neodct_sim_ring` and the phone rings. It
re-checks for real hardware every ten seconds, so a modem plugged in later gets
picked up on its own.

**NotifyService** is the ringer and the little "1 message received" banner.
Short beeps — the text-arrived beep, and the dial-pad tones — are fired off as
throwaway `aplay` processes so nothing ever waits on them. The ringtone is
different: it has to loop until you answer, so it is decoded into memory once
and played in-process. The banner is not a popup; it sits on the home screen
like a Nokia 3310, hides the carrier name while it is showing, turns the
softkey into "Read", and is cleared with the C key. A separate flashing
envelope in the status strip stays until the messages are actually read.

**ClockService** stops the clock reading 1970. The board has no battery-backed
clock, so it boots at the Unix epoch, and a phone that thinks it is 1970 fails
*every* HTTPS certificate at once — that is what the browser's security warnings
were. Two fixes: a floor applied instantly with no network (never allow the
clock to read earlier than the date this image was built, or the last time we
successfully synced, whichever is later), and a real SNTP query once a network
route appears. SNTP is 48 bytes out, 48 bytes back, and the only part we use is
one 32-bit number at byte 40. Anything claiming a date before 2020 or after
2100 is refused as a fault or an attack.

**BatteryService** reads the battery voltage from a MAX1704x "fuel gauge" chip
on the I²C bus. It reads one 16-bit register (`VCELL`, register 0x02), converts
it to volts, averages the last five readings so a momentary sag doesn't move the
gauge, and turns that into the 0–4 bar battery icon. It also enforces the power
policy: at 3.45 V it warns once, at 3.25 V it warns harder, and at 3.20 V — only
after three readings in a row agree — it puts a "Battery empty" notice on screen
and runs `poweroff`. As with the modem, no hardware means Simulation Mode
(a fixed 3.85 V, overridable by a file or an environment variable), and the
home screen honestly draws a `?` next to the battery icon instead of pretending.

---

## Files and where they go in C

| Python | LOC | Purpose | C destination |
| --- | --- | --- | --- |
| `System/core/ModemService/__init__.py` | 1084 | AT engine, URC state machine, CSQ/CEREG/COPS polling, SMS, call control, call audio, simulation | `core/nd_modem.c` + `core/nd_modem_at.c` + `core/nd_modem_audio.c` + `core/nd_modem_sim.c` (+ `nd_modem.h`) |
| `System/core/NotifyService/__init__.py` | 232 | banner state, beeps, looping ringer | `core/nd_notify.c` + `core/nd_ringer.c` (+ `nd_notify.h`) |
| `System/core/ClockService/__init__.py` | 250 | epoch floor, SNTP, `/NeoDCT/User/.clock` | `core/nd_clock.c` (+ `nd_clock.h`) |
| `System/core/BatteryService/__init__.py` | 302 | MAX1704x I²C fuel gauge, smoothing, warning latches | `core/nd_battery.c` (+ `nd_battery.h`) |

Everything above lives **in the core process**, not in `libneodct.so`. Apps
never link against these; they reach the modem and the gauge through the
`ui` handle that the core passes into an app (see
[Public interface](#public-interface-the-functions-other-parts-call) — that
handle becomes an IPC surface in the process-per-app design, and the list of
methods below is exactly what has to cross it).

Non-Python files in this subsystem's blast radius, **unchanged by the port**:

| File | Relationship |
| --- | --- |
| `overlay/etc/init.d/S45modem` (busybox ash, ~690 lines) | Owns *data* bring-up (QMI / `AT$QCRMCALL` / RmNet). Shares `/tmp/neodct-modem.lock` with us. **Stays shell.** |
| `System/engineering/tools/atcmd` (busybox ash) | One-shot AT helper. Shares the same lock. **Stays shell.** |
| `System/engineering/tools/max1704x_watch.py` | Bench tool, calls `quickstart()`. Out of scope (engineering). |
| `System/tones/*.mp3`, `sms.wav`, `dtmf/*.wav` | Data. Unchanged. Formats measured in [External dependencies](#external-dependencies-and-their-c-replacements). |

---

## Behaviour that must be reproduced exactly

### 0. Constants, verbatim

```c
/* --- ModemService ---------------------------------------------------- */
#define ND_MODEM_LOCK_FILE          "/tmp/neodct-modem.lock"
#define ND_ASOUND_DIR               "/proc/asound"
#define ND_SIM_CSQ_FILE             "/tmp/neodct_sim_csq"
#define ND_SIM_RING_FILE            "/tmp/neodct_sim_ring"
#define ND_SIM_OPS_FILE             "/tmp/neodct_sim_operator"
#define ND_SIM_SMS_FILE             "/tmp/neodct_sim_sms"

#define ND_MODEM_DEFAULT_PORT       "AUTO"      /* probes ttyUSB2/3 first */
#define ND_MODEM_BAUD               B115200

#define ND_POLL_URC_S               0.5
#define ND_SMS_PROMPT_TIMEOUT_S     5.0
#define ND_SMS_SEND_TIMEOUT_S       30.0
#define ND_POLL_SIGNAL_S            5.0
#define ND_POLL_NET_S               20.0
#define ND_POLL_OPERATOR_S          60.0
#define ND_PROBE_RETRY_S            10.0
#define ND_CLCC_POLL_S              2.0
#define ND_AUDIO_RESTART_HOLDOFF_S  3.0

/* CSQ rssi (0..31, 99 = unknown) -> 0..4 bars; ~ -105/-93/-81/-73 dBm */
static const int ND_BAR_THRESHOLDS[4] = { 2, 8, 14, 20 };

#define ND_PCM_FORMAT               "S16_LE"
#define ND_PCM_RATE_DEFAULT         16000       /* AT+CPCMFRM=1; 0 => 8000 */

/* internal loop granularity, load-bearing for timing 1:1 */
#define ND_TRANSACT_SLEEP_S         0.02        /* _transact poll gap      */
#define ND_SMS_WAIT_SLEEP_S         0.05        /* CMGS ack poll gap       */
#define ND_PROMPT_SLEEP_S           0.02        /* '>' prompt poll gap     */
#define ND_READ_CHUNK               512         /* os.read(fd, 512)        */
#define ND_PROMPT_CHUNK             64          /* os.read(fd, 64)         */
#define ND_EVENT_QUEUE_MAX          8           /* deque(maxlen=8)         */

/* --- NotifyService --------------------------------------------------- */
#define ND_TONES_DIR                "/NeoDCT/System/tones"
#define ND_SMS_TONE                 "/NeoDCT/System/tones/sms.wav"
#define ND_RING_RATE                44100
#define ND_RING_BUF_MS              500
#define ND_RING_SETTING             "system.audio.ringtone"
/* order is load-bearing */
static const char *ND_RING_FALLBACKS[4] = {
    "/NeoDCT/System/tones/Low.mp3",
    "/NeoDCT/System/tones/Nokia Tune.mp3",
    "/NeoDCT/System/tones/Ring Ring.mp3",
    "/NeoDCT/System/tones/sms.wav",
};
/* extension retry order when the configured tone was renamed/re-encoded */
static const char *ND_RING_EXT_RETRY[5] = { ".mp3", ".wav", ".wma", ".flac", ".ogg" };
/* last-resort directory sweep accepts only these, case-insensitive */
static const char *ND_RING_SWEEP_EXT[3] = { ".mp3", ".wav", ".wma" };

/* --- ClockService ---------------------------------------------------- */
#define ND_NTP_EPOCH_OFFSET         2208988800u  /* 1900-01-01 -> 1970-01-01 */
#define ND_VERSION_PROP             "/NeoDCT/System/version.prop"
#define ND_CLOCK_STATE_FILE         "/NeoDCT/User/.clock"
#define ND_NTP_QUERY_TIMEOUT_S      5
#define ND_NTP_PORT                 123
#define ND_CLOCK_SANE_MIN           1577836800   /* 2020-01-01 */
#define ND_CLOCK_SANE_MAX           4102444800   /* 2100-01-01 */
static const char *ND_NTP_SERVERS[3] = {
    "0.pool.ntp.org", "1.pool.ntp.org", "2.pool.ntp.org",
};
#define ND_CLOCK_ROUTE_TRIES        60           /* x 5 s = 5 minutes */
#define ND_CLOCK_ROUTE_SLEEP_S      5

/* --- BatteryService -------------------------------------------------- */
#define ND_I2C_SLAVE                0x0703       /* ioctl request */
#define ND_REG_VCELL                0x02
#define ND_REG_SOC                  0x04
#define ND_REG_MODE                 0x06
#define ND_REG_VERSION              0x08
#define ND_REG_CONFIG               0x0C
#define ND_REG_CRATE                0x16         /* 17048/49 only */
#define ND_VCELL_LSB                78.125e-6    /* volts per LSB (= 1/12800) */
#define ND_CRATE_LSB                0.208        /* %/hr per LSB, signed */
#define ND_QUICKSTART_MODE          0x4000
static const double ND_LEVEL_THRESHOLDS[4] = { 3.35, 3.55, 3.75, 3.95 };
#define ND_LOW_WARN_V               3.45
#define ND_CRITICAL_WARN_V          3.25
#define ND_SHUTDOWN_V               3.20
#define ND_REARM_HYSTERESIS_V       0.05
#define ND_SHUTDOWN_CONFIRM_SAMPLES 3
#define ND_BATT_POLL_INTERVAL_S     2.0
#define ND_BATT_SMOOTH_WINDOW       5
#define ND_SIM_DEFAULT_VCELL        3.85
#define ND_SIM_ENV_VAR              "NEODCT_BATT_SIM_VCELL"
#define ND_BATT_SIM_FILE            "/tmp/neodct_sim_vcell"
#define ND_BATT_DEFAULT_I2C_BUS     3            /* /dev/i2c-3 */
#define ND_BATT_DEFAULT_I2C_ADDR    0x36
```

Settings keys read by this subsystem (all via `SettingsStorage.get_setting`,
which layers `DEFAULTS < /NeoDCT/User/settings.prop < /NeoDCT/System/version.prop`):

| Key | Default passed by caller | `SettingsStorage.DEFAULTS` entry | Read when |
| --- | --- | --- | --- |
| `system.hw.modem_at_port` | `"AUTO"` | — | once, in the constructor |
| `system.hw.modem_pcm_rate` | `"16000"` | — | once, in the constructor |
| `system.modem.allow_calls` | `"ON"` | — | once, in the constructor |
| `system.hw.modem_pcm_port` | `"AUTO"` | — | **every** `_start_call_audio()` |
| `system.hw.modem_mic_device` | `"AUTO"` | — | **every** `_start_mic_pipe()` |
| `system.audio.ringtone` | `""` | `/NeoDCT/System/tones/Low.mp3` | **every** `start_ring()` |
| `system.hw.battery_i2c_bus` | `3` | `"3"` | once, in the constructor |
| `system.hw.battery_i2c_addr` | `"0x36"` (parsed base 0) | `"0x36"` | once, in the constructor |

> **Quirk to port as-is.** Because `system.audio.ringtone` has a `DEFAULTS`
> entry, `get_setting(RING_SETTING, "")` can never return `""`. The
> `ND_RING_FALLBACKS` walk is therefore only reachable when `settings.prop`
> points at a file that does not exist *and* the same-stem extension retry
> also misses. Do not "simplify" the fallback chain away.

`allow_calls` is truthy for `strip().upper() in ("ON","1","TRUE","YES")` —
anything else, including an unreadable settings file's exception path, is…
careful: the *exception* path returns `True`, the *unrecognised value* path
returns `False`. Reproduce both.

---

### 1. ModemService — construction and hardware probe

Constructor order (`ModemService.__init__`, line 104):

1. Print `[MODEM] Initializing ModemService...`
2. Zero all state: `state="IDLE"`, `hardware=False`, `port=None`, `fd=None`,
   `imei=None`, `operator=None`, `caller_id=None`, `_csq=None`,
   `_reg_stat=None`, `_rxbuf=b""`, event deque empty, **all six `_next_*`
   timers = 0.0** (so the first `poll()` fires CSQ immediately),
   `_sim_connect_at=None`, `_audio_proc=None`, `_mic_proc=None`,
   `_active_pcm_port=None`, `_mic_fails=0`, `_pcm_cleanup=False`,
   `_pcm_retry=False`, `_call_stat=None`, `_call_connected_at=None`,
   `_sim_ring_mtime=None`.
3. Read the three one-shot settings (`_pcm_rate_setting`, `_port_from_settings`,
   `_calls_enabled_setting`). Each swallows exceptions; the port reader prints
   `[MODEM] Settings unavailable ({exc}); probing default ports.` on failure.
4. `open("/tmp/neodct-modem.lock", O_RDWR|O_CREAT, 0666)` — **not** closed until
   process exit. This fd is the flock handle.
5. `_probe_hardware()`. On failure print exactly two lines:
   ```
   [MODEM] HARDWARE NOT FOUND: Running in Simulation Mode.
   [MODEM] Will re-probe every 10s; sim hooks: /tmp/neodct_sim_csq / /tmp/neodct_sim_ring.
   ```
   (the `%ds` is `int(PROBE_RETRY_S)` → `10`).

#### `_probe_hardware()` (line 199)

```
_next_probe = monotonic() + PROBE_RETRY_S     /* set BEFORE trying */
if !flock(LOCK_EX|LOCK_NB): return false      /* S45modem mid-session */
try _probe_ports() finally flock(LOCK_UN)
```
The timer is armed even when the lock grab fails, so a locked port still costs
a full 10 s before the next attempt.

#### `_candidate_ports()` (line 178)

If `system.hw.modem_at_port` is set and is not `"AUTO"`, the list is exactly
that one string. Otherwise:

* `sorted(os.listdir("/sys/class/tty"))` — plain byte-wise sort, so
  `ttyUSB10` sorts *before* `ttyUSB2`. Reproduce with `strcmp` ordering, not a
  natural sort.
* Skip anything not starting with `ttyUSB`.
* Read `/sys/class/tty/<name>/device/../bInterfaceNumber`, `strip()`, parse as
  **hexadecimal** (`int(..., 16)`). Any failure → `iface = None`.
* `iface in (2, 3)` → *preferred* list as `(iface, "/dev/"+name)`.
* `iface != 0` (including `None`) → *rest* list as `/dev/`+name.
* `iface == 0` is dropped entirely (Qualcomm DIAG port; binary, never answers AT).
* Result = `preferred` sorted by `(iface, dev)`, then `rest` in listdir order.

#### `_probe_ports()` (line 209)

For each candidate: skip if `os.path.exists()` is false; `_open_port()` (any
exception → next candidate); **assign `self.fd`/`self.port` before testing**;
`_transact("AT", timeout=1.0)`; if final is `"OK"` → `_init_modem()`, return
true. Otherwise clear `fd`/`port` and `close(fd)`.

> **Port the bug.** If `_transact("AT")` hits a write error it calls
> `_drop_hardware()`, which already closed the fd and set `self.fd = None`; the
> `os.close(fd)` a few lines later then double-closes and the exception is
> swallowed. In C a double `close()` on a recycled fd number is not harmless —
> guard it with a flag, but keep the externally visible behaviour (candidate
> skipped, probe continues) identical.

#### `_open_port()` (line 237) — exact termios

```c
int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
struct termios t;
tcgetattr(fd, &t);            /* start from the current attrs, like Python */
t.c_iflag = 0;
t.c_oflag = 0;
t.c_cflag = CS8 | CREAD | CLOCAL;   /* NB: assignment, not |=  */
t.c_lflag = 0;
cfsetispeed(&t, B115200);
cfsetospeed(&t, B115200);
t.c_cc[VMIN]  = 0;
t.c_cc[VTIME] = 0;
tcsetattr(fd, TCSANOW, &t);
```
Everything else in `c_cc` keeps whatever `tcgetattr` returned. No `tcflush`,
no `tcdrain`, no DTR/RTS handling, no hardware flow control.

#### `_init_modem()` (line 252)

Set `hardware = True`, then in this order, each a `_transact` with the default
2.0 s timeout, **return codes ignored**:

| # | Command | Why (from the source comments) |
| --- | --- | --- |
| 1 | `ATE0` | echo off |
| 2 | `AT+CMEE=2` | verbose errors |
| 3 | `AT+CLIP=1` | caller-ID URCs |
| 4 | `AT+CVHU=0` | make hangup commands actually hang up |
| 5 | `AT+COPS=3,1` | short operator names ("T-Mobile", "Tello") |
| 6 | `AT+CMGF=1` | text-mode SMS everywhere |
| 7 | `AT+CNMI=2,1,0,0,0` | push `+CMTI` URC on new SMS |
| 8 | `AT+CGSN` (timeout **2.0**) | IMEI |

IMEI extraction: from the collected lines take those whose `strip()` is
all-digits; `imei = digits[0].strip()` or `None`. Then print
`[MODEM] SIM7600 on %s (IMEI %s). Using REAL modem.` with `"unknown"` when the
IMEI is missing. Then **queue `("sms_stored_check", None)`**. If
`allow_calls` is false, additionally print
`[MODEM] Real call placement DISABLED (system.modem.allow_calls=OFF); dial/answer will simulate.`

---

### 2. ModemService — the AT engine

#### `_read_pending()` (line 290)

```
loop: chunk = read(fd, 512); if chunk empty -> break; rxbuf += chunk
  EAGAIN/EWOULDBLOCK -> stop reading, keep going
  any other errno    -> _drop_hardware("port read failed: <strerror>"); return no lines
split rxbuf on b"\n"; for each complete line:
    decode ASCII with U+FFFD replacement, strip() whitespace
    keep only non-empty lines
```
The trailing partial line stays in `rxbuf`. `\r` is removed by `strip()`.
There is **no cap on `rxbuf`** — a babbling port grows it without bound; see
[Risks](#risks).

#### `_transact(cmd, timeout=2.0)` (line 311)

1. If `fd` is NULL → `(None, [])`.
2. Drain stale input first: every line from `_read_pending()` goes through
   `_handle_urc()` and is **discarded** (not collected).
3. `write(fd, cmd + "\r")`. Write error → `_drop_hardware("port write failed: …")`,
   return `(None, [])`.
4. Deadline = `monotonic() + timeout`. Loop while `monotonic() < deadline`:
   * for each line from `_read_pending()`:
     - if line is exactly one of `FINAL_CODES = {"OK","ERROR","NO CARRIER","NO DIALTONE","BUSY","NO ANSWER"}`
       **or** starts with `"+CME ERROR"` **or** `"+CMS ERROR"`:
       - if the line is one of `"NO CARRIER"`, `"BUSY"`, `"NO ANSWER"` → also
         run `_handle_urc(line)` (a mid-command call teardown is a URC too);
       - **return** `(line, collected)`. The final line is *not* appended.
     - if the line starts with any of
       `URC_PREFIXES = {"RING", "+CLIP:", "VOICE CALL:", "MISSED_CALL:", "NO CARRIER", "+CMTI:", "+CEREG:", "+CREG:", "+CPIN:", "+SIMCARD:"}`
       → run `_handle_urc(line)` **and then still append it to `collected`**.
       This double-handling is deliberate: it is how `AT+CEREG?`'s own reply
       gets parsed into `_reg_stat`.
     - append the line to `collected`.
   * `sleep(0.02)`
5. On timeout return `(None, collected)` — a `None` final code means
   "timed out or busy" everywhere downstream.

#### `_command(cmd, timeout)` (line 344)

`if !hardware -> (None, [])`; `if !flock(LOCK_EX|LOCK_NB) -> (None, [])`;
else `_transact` inside try/finally with `flock(LOCK_UN)`.

#### `_drop_hardware(why)` (line 355)

Print `[MODEM] Lost the modem ({why}); back to Simulation Mode.`, close the fd
(swallowing errors), then set `fd=None, port=None, hardware=False,
state="IDLE", _csq=None, _reg_stat=None, operator=None, _rxbuf=b""`, and queue
`("modem_lost", why)`. Note `caller_id`, `imei`, `_call_stat` and the audio
processes are **not** cleared here.

---

### 3. ModemService — URC dispatch (`_handle_urc`, line 374)

Console echo first: any line starting with `RING`, `VOICE CALL:`, `NO CARRIER`,
`MISSED_CALL`, `+CLIP:`, `BUSY`, or `NO ANSWER` is printed as `[MODEM] {line}`.
(Note `MISSED_CALL` here has no colon, unlike the prefix tuple.)

Then, first match wins:

| Match | Effect |
| --- | --- |
| line **==** `"RING"` | if `state != "RINGING"`: `state = "RINGING"`, `caller_id = None`, queue `("incoming", None)` |
| starts `"+CLIP:"` | `number = line.split('"')[1]` (IndexError → `None`); if number and `caller_id != number`: `caller_id = number`, queue `("incoming", number)` |
| starts `"VOICE CALL: BEGIN"` | `state = "CONNECTED"`; if `_call_connected_at is None` set it to `monotonic()`; `_pcm_retry = True`; queue `("connected", caller_id)` |
| starts `"VOICE CALL: END"` **or** line == `"NO CARRIER"` | if `state != "IDLE"`: `state = "IDLE"`, `_stop_call_audio()`, queue `("ended", line)` |
| starts `"MISSED_CALL:"` | `state = "IDLE"`, `_stop_call_audio()`, queue `("missed", <text after the first ':' , stripped>)` — unconditionally, no `state != IDLE` guard |
| starts `"+CMTI:"` | `index = int(line.rsplit(",", 1)[1])`; parse failure → **silently return**; else queue `("sms_received", index)` |
| starts `"+CEREG:"` or `"+CREG:"` | `_parse_reg(line)` |

`_parse_reg` (line 419): take everything after the first `:`, split on `,`.
If there are ≥2 fields use field **1**, else field **0**. `int()` with
Python's leading/trailing-whitespace tolerance (use `strtol`, which skips
leading whitespace; reject on no digits). Parse failure leaves `_reg_stat`
unchanged. This handles both `+CEREG: 0,1` (query) and `+CEREG: 1`
(unsolicited), and `+CEREG: 0,1,"1A2B","01234567",7` correctly.

**Event queue semantics.** `collections.deque(maxlen=8)`. `append` on a full
deque silently drops the **oldest** (leftmost); `appendleft` (used by
`requeue_event`) on a full deque silently drops the **newest** (rightmost).
`take_pending_event()` pops from the left. A C ring buffer must match both
overflow directions.

---

### 4. ModemService — `poll()` (line 429), the tick

Called from `NeoDCT_UI._modem_tick()`, which is called from
`NeoDCT_UI.read_keypress()`, which every screen funnels through — nominally
**~10 Hz** (the core loop and every app pass `timeout=0.1`). Rate limiting is
entirely inside `poll()`.

```
now = monotonic()

/* Pretend-dial completion: simulation mode AND allow_calls=OFF on real hw */
if _sim_connect_at and state == "CALLING" and now >= _sim_connect_at:
        _sim_connect_at = None
        state = "CONNECTED"
        if _call_connected_at is None: _call_connected_at = monotonic()
        queue ("connected", None)

if not hardware:
        _poll_sim(now)          /* NOT rate-limited: runs at full tick rate */
        return

if now < _next_urc: return
_next_urc = now + 0.5           /* => 2 Hz */

if not flock(LOCK_EX|LOCK_NB): return
/* --- lock held from here --- */
for line in _read_pending(): _handle_urc(line)

if _pcm_cleanup:  _pcm_cleanup = false; _transact("AT+CPCMREG=0", 2.0)

if state != "IDLE":
        if _pcm_retry:
                _pcm_retry = false
                final = _transact("AT+CPCMREG=1", 3.0)
                print "[MODEM] CPCMREG=1 (in-call retry) -> {final}"
        if now >= _next_clcc:
                _next_clcc = now + 2.0
                _poll_clcc()
        _watch_audio_proc(now)

/* if/elif/elif -- at most ONE query per tick, deliberately staggered */
if      now >= _next_csq:  _next_csq  = now + 5.0;  final,lines = _transact("AT+CSQ",   1.5); if OK: _parse_csq(lines)
elif    now >= _next_net:  _next_net  = now + 20.0; _transact("AT+CEREG?", 1.5)   /* reply parsed as a URC */
elif    now >= _next_cops: _next_cops = now + 60.0; final,lines = _transact("AT+COPS?", 3.0); if OK: _parse_cops(lines)
flock(LOCK_UN)
```

**Start-up schedule, exactly.** All three timers start at 0.0, so:
tick 1 → `AT+CSQ`; tick 2 (+0.5 s) → `AT+CEREG?`; tick 3 (+1.0 s) → `AT+COPS?`;
from then on each runs on its own cadence, still at most one per tick.

`_parse_csq` (line 568): for each line starting `+CSQ:`, `_csq = int(text
after ':' up to the first ',')`. Loops over all lines — the last match wins.
Parse failure leaves `_csq` unchanged.

`_parse_cops` (line 576): for each line starting `+COPS:`,
`operator = line.split('"')[1] if '"' in line else None`. Again the last match
wins, and a quote-less reply (`+COPS: 0`) **sets operator to None**.

#### `_poll_clcc()` (line 489)

`_transact("AT+CLCC", 2.0)`; non-`OK` → return. Scan the collected lines for the
first that starts `+CLCC:` and parse `int(split(":",1)[1].split(",")[2])` — the
`<stat>` field; a parse error `continue`s to the next line, a success `break`s.

* No `+CLCC:` line at all and `state != "IDLE"` → print
  `[MODEM] CLCC: no call in the list; ending call state.`, `state = "IDLE"`,
  `_stop_call_audio()`, queue `("ended", "CLCC empty")`.
* `stat` changed → print `[MODEM] Call progress: {name}` using
  `CLCC_STATES = {0:"CONNECTED",1:"HELD",2:"CALLING",3:"RINGING",4:"INCOMING",5:"WAITING"}`,
  falling back to the raw integer.
* `stat == 0` → set `_call_connected_at` if unset, `state = "CONNECTED"`.

#### `_watch_audio_proc(now)` (line 523)

Return immediately if `_active_pcm_port is None` or `now < _next_audio_restart`.

* Speaker (`_audio_proc`) died → arm holdoff `now + 3.0`, print
  `[MODEM] Speaker pipe exited rc={rc} mid-call; restarting.`, clear the
  handle, `_start_speaker_pipe(port)`. **Retries forever.**
* Mic (`_mic_proc`) died → clear handle, `_mic_fails += 1`.
  * `_mic_fails >= 3` → print
    `[MODEM] Mic pipe keeps dying (rc={rc}); giving up -- call continues listen-only. Check `arecord -l` and the system.hw.modem_mic_device setting.`
    and **stop retrying for the rest of the call**.
  * else arm holdoff, print `[MODEM] Mic pipe exited rc={rc}; retrying.`,
    `_start_mic_pipe(port)`.

Note both branches run in the same call, and both may re-arm the same holdoff.

#### `call_status()` (line 553) — drives the in-call screen text

```
state == "IDLE"                       -> ("IDLE", None)
_call_stat == 3                       -> ("RINGING", None)
_call_stat == 2                       -> ("CALLING", None)
state == "CONNECTED" or _call_stat==0 -> ("CONNECTED", int(monotonic() - _call_connected_at) or None)
otherwise                             -> (state, None)
```
`call_screen.py` maps `CALLING → "Calling..."`, `RINGING → "Ringing..."`,
anything else → `"Call 1"`, and draws `mm:ss` from the seconds via
`"%02d:%02d" % (secs // 60, secs % 60)`.

---

### 5. ModemService — Simulation Mode (`_poll_sim`, line 581)

Runs on **every** tick (~10 Hz), not the 2 Hz URC cadence.

**Ring hook.** If `/tmp/neodct_sim_ring` exists: `mtime = getmtime()` (OSError →
`None`). If `state == "IDLE"` **and** `mtime != _sim_ring_mtime`:
store the new mtime, read the file, `strip()` it, use `"5550000"` if empty or
unreadable, set `state = "RINGING"`, queue `("incoming", caller_id)`.
If the file does **not** exist: `_sim_ring_mtime = None`, and if
`state == "RINGING"` → `state = "IDLE"`, queue `("ended", "sim caller gave up")`.

**SMS hook.** If `/tmp/neodct_sim_sms` exists: read it, `strip()`, `os.remove()`
it (all inside one try — a read failure still leaves `content` at whatever was
read, and the remove may not happen). If content is non-empty: split on the
**first** `|`; if there is no `|` (empty body) then `sender = "5550000"` and the
whole string becomes the body. Queue `("sms_sim", (sender.strip(), body.strip()))`.

**Re-probe.** `if now >= _next_probe and _probe_hardware(): queue ("modem_found", port)`.

**Signal / operator hooks** are *not* in `_poll_sim`; they are read on demand:
`signal_level()` opens `/tmp/neodct_sim_csq` and `_bars(int(text))` on every
call, `operator_display()` opens `/tmp/neodct_sim_operator` on every call. Both
return `None` on any exception. Since these are called once per rendered home
frame, in C that is an `open`/`read`/`close` per frame — keep it (it is how the
hook stays live) but it is a syscall cost worth noting.

---

### 6. ModemService — call audio over USB

#### `_pcm_port()` (line 628)

`system.hw.modem_pcm_port`; if not `"AUTO"` use it verbatim. Otherwise scan
`sorted(os.listdir("/sys/class/tty"))` for `ttyUSB*` whose
`device/../bInterfaceNumber` parses (hex) as **4**; first hit wins. If nothing
matches, return the literal `"/dev/ttyUSB4"`.

#### `_start_call_audio()` (line 677)

Return immediately if either process handle is non-NULL. Resolve the port; if
it does not exist print `[MODEM] PCM port {port} not found; call audio unavailable.`
and return. Then **open the PCM port purely to configure it and close it again**:

```c
int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
tcgetattr(fd, &t);
t.c_iflag = t.c_oflag = t.c_lflag = 0;      /* fully raw: PCM is not text */
t.c_cflag = CS8 | CREAD | CLOCAL;           /* note: no cfsetspeed here   */
tcsetattr(fd, TCSANOW, &t);
close(fd);
```
Any exception → print `[MODEM] PCM port setup failed: {exc}` and return without
starting anything. On success: `_active_pcm_port = port`, `_mic_fails = 0`,
then `_start_speaker_pipe(port)` and `_start_mic_pipe(port)`.

> Note the asymmetry with `_open_port`: the PCM port's baud is **not** set.
> Port as-is.

#### `_start_speaker_pipe(port)` (line 707)

```
argv = ["aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "<pcm_rate>", "-c", "1", <port>]
stdout = /dev/null, stderr = /dev/null, no shell
```
Success → print `[MODEM] Call audio: aplay <- {port} ({rate} Hz S16_LE).`
Failure → handle NULL, print `[MODEM] Speaker pipe unavailable: {exc}`.

#### `_start_mic_pipe(port)` (line 721)

Read `system.hw.modem_mic_device`, `.strip()`.
* Upper-cased in `("", "OFF", "NONE")` → print
  `[MODEM] Mic uplink disabled (system.hw.modem_mic_device=OFF).` and return.
* Upper-cased `"AUTO"` → `_find_capture_device()`; `None` → print
  `[MODEM] No ALSA capture device found (arecord -l); call is listen-only.`
  and return; otherwise print `[MODEM] Mic auto-detected: {device}`.
* Otherwise the string is used verbatim as the ALSA device name.

```
argv = ["arecord", "-q", "-t", "raw", "-f", "S16_LE", "-r", "<pcm_rate>", "-c", "1", "-D", <device>, <port>]
```
Success → `[MODEM] Mic uplink: arecord -D {device} -> {port} ({rate} Hz S16_LE).`
Failure → handle NULL, `[MODEM] Mic uplink unavailable ({exc}); call is listen-only.`

#### `_find_capture_device()` (line 656)

```
for entry in sorted(listdir("/proc/asound")):
    if not (entry startswith "card" and entry[4:] is all digits): continue
    for node in sorted(listdir("/proc/asound/<entry>")):
        if node startswith "pcm" and node ends with "c" and node[3:-1] is all digits:
            return "plughw:<entry[4:]>,<node[3:-1]>"
return None
```
Both listings are byte-sorted, so `pcm10c` precedes `pcm0c`… no: `"pcm0c"` <
`"pcm10c"` byte-wise, so `pcm0c` wins — but `card10` precedes `card2`. Keep
`strcmp` ordering exactly. Any exception anywhere → `None`.

#### `_stop_call_audio()` (line 746)

For `_audio_proc` then `_mic_proc`: `kill()` (SIGKILL, not SIGTERM), `wait(timeout=1)`,
swallow errors, set handle NULL, remember that something was stopped. If
anything was stopped print `[MODEM] Call audio stopped.` Then unconditionally:
`_active_pcm_port = None`, `_call_stat = None`, `_call_connected_at = None`,
and `if hardware: _pcm_cleanup = True`.

---

### 7. ModemService — call control

#### `dial(number)` (line 770)

1. Filter the number to the character set `0123456789*#+` (everything else
   dropped, including spaces, dashes and parentheses).
2. Print `[MODEM] Requesting Dial: {number}`. **This print happens after
   filtering but before the empty check**, so an all-junk number logs an empty
   dial and then returns `False`.
3. Empty → return `False`.
4. `not hardware or not allow_calls` → pretend: if `hardware` also print
   `[MODEM] Calls not enabled yet; simulating this dial.`; then
   `state = "CALLING"`, `_sim_connect_at = monotonic() + 2.0`, return `True`.
5. Real: `_command("ATD{number};", timeout=8.0)`. Non-`OK` → print
   `[MODEM] Dial failed (final={final})`, return `False`.
6. On `OK`: `state = "CALLING"`, `_call_stat = None`, `_call_connected_at = None`,
   `_next_clcc = 0.0`. Then `frm = "1" if pcm_rate == 16000 else "0"`,
   `_command("AT+CPCMFRM=" + frm, 2.0)`, `_command("AT+CPCMREG=1", 3.0)`, print
   `[MODEM] USB audio setup: CPCMFRM={frm} -> {final_frm}, CPCMREG=1 -> {final_reg}`.
   If `final_reg != "OK"` set `_pcm_retry = True`. Then `_start_call_audio()`.
   Return `True`.

> Each `_command` takes and releases the flock separately, so `S45modem` can
> interleave between `ATD` and `CPCMREG`. Port as-is.

#### `answer()` (line 802)

`not hardware or not allow_calls` → `state = "CONNECTED"`, return `True`.
Otherwise `_command("ATA", 8.0)`; on `OK` → `state = "CONNECTED"`,
`_command("AT+CPCMREG=1", 3.0)`, `_start_call_audio()`, return `True`;
else return `False` **without changing state**.

#### `hangup()` (line 814)

Print `[MODEM] Requesting Hangup`. `_sim_connect_at = None`. `_stop_call_audio()`.
If not hardware → `state = "IDLE"`, return `True`.
Else `_command("AT+CHUP", 5.0)`; if that is not `"OK"` then
`_command("ATH", 5.0)`. Set `state = "IDLE"` unconditionally and return
`final == "OK"` where `final` is whichever command ran last.

---

### 8. ModemService — SMS

#### `send_sms(number, text)` → `(ok, detail)` (line 831)

1. Filter number to `0123456789*#+`. Strip `\x1a` (Ctrl-Z) and `\x1b` (ESC)
   from the text — **and nothing else**; newlines are kept.
2. Empty number → `(False, "no number")`. Empty text → `(False, "empty message")`.
3. Print `[MODEM] Sending SMS to {number} ({len} chars)`.
4. No hardware → print `[MODEM] (Simulation Mode: pretending the SMS went out.)`,
   queue `("sms_sent", number)`, return `(True, "simulated")`.
5. `flock(LOCK_EX|LOCK_NB)` fails → `(False, "modem port busy")`.
6. Inside the lock:
   * `_transact("AT+CMGF=1")` (2.0 s). Non-`OK` → `(False, "text mode rejected (%s)" % final)`.
   * `write(fd, 'AT+CMGS="' + number + '"\r')`. Error → `_drop_hardware(...)`,
     `(False, "modem lost")`.
   * `_wait_sms_prompt(5.0)`. Failure → write a single `\x1b` (ESC) to back out
     of the half-open CMGS (errors ignored), return `(False, "no > prompt from modem")`.
   * `write(fd, text_as_ascii_with_'?'_replacement + "\x1a")`. Error →
     `_drop_hardware(...)`, `(False, "modem lost")`.
     *(Python's `encode("ascii","replace")` substitutes `?` for every
     non-ASCII code point. Reproduce byte-for-byte: any byte ≥ 0x80 or any
     non-ASCII source character becomes `'?'`.)*
   * Wait up to 30 s, polling every 0.05 s, over `_read_pending()`:
     - `+CMGS:` → `ref = text after the first ':' , stripped` (keep looping)
     - `OK` → print `[MODEM] SMS accepted by network (ref {ref}).`, queue
       `("sms_sent", number)`, return `(True, ref or "sent")`
     - `ERROR` / `+CMS ERROR…` / `+CME ERROR…` → print `[MODEM] SMS rejected: {line}`,
       return `(False, line)`
     - a `URC_PREFIXES` line → `_handle_urc(line)` and keep waiting
     - anything else is ignored
   * Timeout → `(False, "timeout waiting for network")`.
7. `flock(LOCK_UN)` in the finally.

#### `_wait_sms_prompt(timeout)` (line 976)

Poll every 0.02 s until the deadline:
* `read(fd, 64)` appended to `rxbuf`; `EAGAIN` → carry on; other OSError →
  return `False`.
* Drain **complete** lines out of `rxbuf`: `ERROR` / `+CMS ERROR…` / `+CME ERROR…`
  → return `False`; `URC_PREFIXES` lines → `_handle_urc`; anything else is
  discarded.
* If `b">"` appears anywhere in what is left of `rxbuf` → **clear `rxbuf`
  entirely** (swallowing the prompt and the space after it) and return `True`.
* Deadline → `False`.

#### `_parse_sms_records(lines, header)` (line 897, static)

Walks the reply lines building `[{index, sender, body}]`:
* A line starting with `header` (`"+CMGR:"` or `"+CMGL:"`) closes the previous
  record and starts a new one.
  * `quoted = line.split('"')[1::2]` — the odd-indexed pieces, i.e. the contents
    of the quoted fields. `sender = quoted[1]` if there are ≥2, else `"unknown"`.
    (For `+CMGR: "REC UNREAD","+15551234","",...` that is the **second** quoted
    field, the number. For `+CMGL: 3,"REC UNREAD","+15551234",...` it is also
    the second quoted field.)
  * For `+CMGL:` only, `index = int(text after ':' up to the first ',')`;
    parse failure leaves it `None`. For `+CMGR:` the index is always `None`
    here and is patched in by the caller.
* Any other line, once a record is open, is appended to that record's body.
  Lines before the first header line are discarded.
* Finally each record's body becomes `"\n".join(body_lines).strip()`.

#### `fetch_sms(index)` → `(status, record)` (line 923)

`not hardware` → `("error", None)`. Lock busy → `("busy", None)`.
Inside the lock: `_transact("AT+CMGF=1")`; `_transact("AT+CMGR=%d" % index, 5.0)`;
non-`OK` → print `[MODEM] CMGR {index} failed ({final})` and `("error", None)`;
no records parsed → `("error", None)`; otherwise
`_transact("AT+CMGD=%d" % index, 5.0)` (**deletes it from the SIM**), take
`records[0]`, force `record["index"] = index`, print
`[MODEM] SMS received from {sender} ({len(body)} chars)`, return `("ok", record)`.

#### `read_stored_sms()` → `(status, records)` (line 951)

`not hardware` → `("error", [])`. Lock busy → `("busy", [])`.
Inside the lock: `_transact("AT+CMGF=1")`; `_transact('AT+CMGL="REC UNREAD"', 8.0)`;
non-`OK` → `("error", [])`; parse with header `"+CMGL:"`; for each record with a
non-`None` index run `_transact("AT+CMGD=%d" % index, 5.0)`; if any records
print `[MODEM] Imported {n} stored SMS from the SIM.`; return `("ok", records)`.

---

### 9. ModemService — readouts

```
registered()      -> _reg_stat in (1, 5)          /* 1 home, 5 roaming */

signal_level()    -> if not hardware:
                         read /tmp/neodct_sim_csq, return _bars(int(text));
                         any failure -> None      /* layout keeps its sim_val */
                     if _reg_stat is not None and not registered(): return 0
                     return _bars(_csq)

_bars(csq)        -> 0 if csq is None or csq == 99
                     else count of {2,8,14,20} that are <= csq   /* 0..4 */

operator_display()-> if not hardware:
                         read /tmp/neodct_sim_operator, strip(); "" -> None;
                         any failure -> None
                     if not registered(): return None
                     return self.operator                        /* may be None */

status_snapshot() -> dict{hardware, port, imei, state, csq, bars, reg_stat,
                          registered, operator, caller_id}
                     /* "bars" re-enters signal_level(), so it hits the sim
                        file again in Simulation Mode */
```

CSQ→bars table, spelled out: `0–1 → 0`, `2–7 → 1`, `8–13 → 2`, `14–19 → 3`,
`20–31 → 4`, `99 → 0`. The engineering Modem app also renders
`dBm = -113 + 2*csq`.

---

### 10. NotifyService — banner state

```
post_sms(row_id, tone=True):  _kind = "sms"; _count += 1; _latest_data = row_id
                              if tone: play_tone(SMS_TONE)
active()      -> _kind is not None
kind()        -> _kind
count()       -> _count
latest_data() -> _latest_data
banner_lines()-> () unless _kind == "sms", else
                 ("%d message"  % 1,  "received")   when count == 1
                 ("%d messages" % n,  "received")   otherwise
dismiss()     -> _kind = None; _count = 0; _latest_data = None
```

Nothing here is time-based and nothing is persisted. A reboot clears the banner;
the unread count on the home screen comes from a separate SQL query in
`main.py` (`SELECT COUNT(*) FROM inbox WHERE is_read = 0`), not from here.

**Where the banner lands on screen** (owned by `spec-core-loop.md`, repeated
here because it is the visible half of this service, `main.py:882–893`):

* Flashing envelope, drawn whenever `notify.active()` **or** `_unread_sms > 0`,
  visible when `int(time.time() * 2) % 2 == 0` — a 0.5 s on / 0.5 s off square
  wave phase-locked to wall-clock, **not** to a monotonic timer.
  Sprite `/NeoDCT/System/ui/resources/img/envelope.png`, loaded through
  `get_image(path, scale=175/240)` (LANCZOS), pasted with its own alpha at
  `(int(46 * 0.7291666…) + 7, int(10 * 0.7291666…))` = **(40, 7)**.
* Banner text, only while `notify.active()`: `y = max(46, int(145 * 0.34))` =
  **49**; each line drawn at `x = 30` in `font_n` (18 px… see the core-loop
  spec: `font_n` is `font.ttf` at **20 px**), colour white, `y += 24` between
  lines. So line 1 at (30, 49) and line 2 at (30, 73).
* While the banner is up, the home layout's `"No Service"` text element is
  **skipped entirely** (the carrier line makes room for it).
* The softkey reads `"Read"` instead of `"Menu"`.

---

### 11. NotifyService — audio

#### `play_tone(path)` (line 101)

`os.path.exists(path)` false → print `[NOTIFY] Tone missing: {path}` and return
`False`. Otherwise spawn `["aplay", "-q", path]` with stdout and stderr to
`/dev/null`, **no wait, no reaping**, return `True`. Any exception → print
`[NOTIFY] Tone playback unavailable: {exc}` and return `False`.

> There is no `waitpid()` anywhere for these. In Python the `Popen` object is
> dropped immediately and CPython's `subprocess` module reaps opportunistically.
> **In C this must become an explicit reaper** — a `SIGCHLD` handler doing
> `waitpid(-1, NULL, WNOHANG)` in a loop, or `signal(SIGCHLD, SIG_IGN)` — or
> every dial-pad keypress leaks a zombie. Note the core also `waitpid()`s for
> app children, so `SIG_IGN` is *not* an option; use the WNOHANG loop and route
> unknown pids to a drop list.

Callers: `post_sms` (sms.wav), and `NeoDCT_UI._play_dtmf(char)` which maps
`*`→`star`, `#`→`hash`, digits to themselves, and plays
`/NeoDCT/System/tones/dtmf/{name}.wav` on every dial-pad keypress on HOME /
HOME_DIALING.

#### `ringtone_path()` (line 119)

1. `configured = str(get_setting("system.audio.ringtone", "")).strip()`.
   An exception prints `[NOTIFY] Ringtone setting unreadable ({exc}).` and
   leaves `configured = None`.
2. `configured` non-empty and the file exists → return it.
3. `configured` non-empty but missing → print `[NOTIFY] Ringtone missing: {path!r}`
   (Python `repr`, i.e. with quotes), then try the same stem with
   `.mp3, .wav, .wma, .flac, .ogg` in that order; first hit prints
   `[NOTIFY] Using {path} instead.` and is returned.
4. Walk `ND_RING_FALLBACKS`; first that exists prints
   `[NOTIFY] Falling back to ringtone {path}.` and is returned.
5. Last resort: `sorted(os.listdir("/NeoDCT/System/tones"))`, first name whose
   lower-cased form ends in `.mp3`/`.wav`/`.wma`, returned as a full path,
   **with no log line**.
6. Otherwise `None`.

#### `start_ring()` (line 155)

`stop_ring()` first (idempotent). `ringtone_path()`; `None` → print
`[NOTIFY] No ringtone available; ringing silently.` and return `False`.

miniaudio path:
```
decoded = miniaudio.decode_file(path, output_format=SIGNED16,
                                nchannels=2, sample_rate=44100)
device  = miniaudio.PlaybackDevice(output_format=SIGNED16, nchannels=2,
                                   sample_rate=44100, buffersize_msec=500,
                                   app_name="NeoDCT Ring")
generator = _loop_generator(decoded.samples); next(generator)
device.start(generator)
print "[NOTIFY] Ringing: {path}"
```
Any exception → print `[NOTIFY] miniaudio ring failed ({exc}); trying mpv.` and
fall through to:
```
Popen(["mpv", "--no-video", "--quiet", "--loop-file=inf", path], DEVNULL, DEVNULL)
print "[NOTIFY] Ringing (mpv): {path}"
```
Failure there → print `[NOTIFY] Ringer unavailable: {exc}`, return `False`.

`_loop_generator(samples)` (line 194) is the whole looping logic. `samples` is
a flat array of `int16` (interleaved stereo). The protocol: the first `yield`
returns an empty buffer and receives `required`, a **frame** count; thereafter
each iteration must supply exactly `required * 2` int16 samples, wrapping the
read position back to zero when it runs off the end:
```
total = len(samples); pos = 0
required = (first yield of b"")
loop:
    frames = required * 2
    chunk  = samples[pos : pos + frames]
    pos   += frames
    if len(chunk) < frames:          /* wrap */
        pos   = frames - len(chunk)
        chunk = chunk + samples[0 : pos]
    if total == 0: return            /* empty file -> stop */
    required = (yield chunk)
```
There is no gap, no fade and no silence between repeats — the loop is sample-exact.

#### `stop_ring()` (line 211) / `ringing()` (line 231)

miniaudio device → `close()` (errors swallowed), handle NULL, print
`[NOTIFY] Ringer stopped.`
mpv process → `terminate()` (SIGTERM), `wait(timeout=0.3)`; on any failure
`kill()` (SIGKILL, errors swallowed); handle NULL, print
`[NOTIFY] Ringer stopped.` — so if **both** were somehow live the line prints
twice. `ringing()` is true if either handle is non-NULL.

The ringer is started and stopped only by
`NeoDCT_UI.handle_incoming_call()` (`main.py:1075`): `start_ring()` before
`incoming_ui.show_incoming()`, `stop_ring()` right after it returns, and
`stop_ring()` again in the `finally`.

---

### 12. ClockService

Started exactly once, from `launcher.py:main()` **before the framebuffer is
opened and before RemoteShell**, as `ClockService.start()` with all defaults.
Any exception there prints `[CLOCK] clock service unavailable: {exc}` and boot
continues. It is *not* imported by `main.py` and has no per-tick work.

#### `_read_prop(path, key)` (line 65)

Line-oriented: the first line that `startswith(key + "=")` yields
`line.split("=", 1)[1].strip()`. `OSError` → `None`. No comment handling, no
whitespace tolerance before the key.

* `build_epoch()` → `int(_read_prop(VERSION_PROP, "system.os.buildepoch"))`,
  `None` on absence or non-integer. Written at build time by
  `neodct/scripts/post-build-system-metadata.sh` (honours `SOURCE_DATE_EPOCH`).
* `last_known()` → `int(open(STATE_FILE).read().strip())`, `None` on
  `OSError`/`ValueError`.
* `remember(when)` → `makedirs(dirname, exist_ok=True)`, write `"%d\n"` to
  `STATE_FILE + ".tmp"`, `flush()`, `fsync()`, `os.replace()` onto `STATE_FILE`.
  Returns `False` on `OSError`. **The atomic rename and the fsync are both
  load-bearing** — this file lives on the only writable partition and survives
  power loss mid-write.

#### `set_clock(when, reason)` (line 109)

Always logs, before doing anything:
```
[CLOCK] setting time (<reason>): <YYYY-MM-DD HH:MM:SS of the OLD time, UTC> -> <YYYY-MM-DD HH:MM:SS UTC of the NEW time>
```
(the first timestamp has **no** trailing `UTC`, the second does; both are
`gmtime`). Then `subprocess.call(["date", "-s", "@%d" % int(when)])` with
stdout/stderr to `/dev/null`; `OSError` → return `False`. Then
`subprocess.call(["hwclock", "-w"])`, same redirection, `OSError` ignored.
Return `True`.

There is no `/etc/TZ` and no `/etc/localtime` in the overlay, so the phone runs
**UTC**; busybox `hwclock -w` with no `/etc/adjtime` writes UTC too. The home
screen's `time.strftime("%H:%M")` is therefore UTC.

#### `apply_floor()` (line 140)

```
floor = max(x for x in (build_epoch(), last_known(), 0) if x is not None)
if not floor: return None            /* 0 or a negative build epoch */
if time() >= floor: return None      /* clock already ahead: leave it */
set_clock(floor, reason="floor: build date or last sync")
return floor
```

#### `query(server, timeout=5)` (line 155)

```
packet = 0x1B followed by 47 zero bytes      /* LI=0, VN=3, Mode=3 (client) */
UDP socket, AF_INET, SOCK_DGRAM, settimeout(timeout)
sendto(packet, (server, 123)); data = recvfrom(48)
len(data) < 48                    -> OSError "short NTP reply from <server>"
seconds = big-endian uint32 at data[40:44]
seconds == 0                      -> OSError "<server> sent a zero timestamp"
epoch = seconds - 2208988800
not (1577836800 <= epoch <= 4102444800) -> OSError "<server> sent an implausible time (<epoch>)"
return epoch
```
The socket is closed in a `finally`. The fractional part at `[44:48]` is
deliberately ignored. `AF_INET` only — **there is no IPv6 path here**, which
matters because the phone's data bearer is IPv6-only with NAT64/DNS64
(see `S45modem`); the NTP query relies on DNS64 synthesising a AAAA…
no: it relies on `getaddrinfo` returning an A record and the socket being v4.
Flagged in [Risks](#risks).

#### `sync(servers, timeout)` (line 182)

Try each server in order; on `OSError`/`socket.error` continue to the next.
First success → `set_clock(when, reason="NTP from %s" % server)`,
`remember(when)`, return the epoch. All failed → `None`.

#### `start(background=True, servers=DEFAULT_SERVERS)` (line 195)

`apply_floor()` **synchronously** (it needs no network and the browser's TLS
depends on it), then a daemon thread named `"clock-sync"` running:
```
for _ in range(60):
    if _has_route(): break
    sleep(5)
else:
    print "[CLOCK] no route after 5 minutes; keeping <YYYY-MM-DD of now>"
    return
when = sync(servers)
if when: print "[CLOCK] synced: <YYYY-MM-DD HH:MM:SS UTC>"
else:    print "[CLOCK] no NTP server answered; keeping <YYYY-MM-DD of now>"
```
Note the loop checks the route **first** and sleeps after, so a route already
present at boot means no delay at all. Worst case the thread lives 300 s and
then exits.

#### `_has_route()` (line 231)

```
/proc/net/route : read whole file, splitlines()[1:]  (skip header)
                  fields = line.split(); len > 2 and fields[1] == "00000000" -> true
/proc/net/ipv6_route : iterate every line (no header)
                  fields = line.split(); len > 1 and fields[0] == 32*"0" and fields[1] == "00" -> true
OSError on either file is ignored; both missing -> false
```

---

### 13. BatteryService

#### Construction (line 60)

Print `[BATT] Initializing BatteryService...`. If either `bus` or `addr` is
`None`, read both from settings (`int(bus)`, `int(addr, 0)` — base 0, so
`0x36` parses as hex and `54` as decimal); on exception print
`[BATT] Settings unavailable ({exc}); using i2c defaults.` and use `3` / `0x36`.
Then `_probe_hardware()`, then `poll(force=True)` so the first home frame is real.

Initial state: `_samples` empty (window 5), `_smoothed = None`,
**`_level = 3`** (matches the pre-0.2.4a static gauge until the first poll),
`_low_armed = True`, `_crit_armed = True`, `_pending_warning = None`,
`_shutdown_count = 0`, `_last_poll = 0.0`, `_read_error_streak = 0`.

#### `_probe_hardware()` (line 100)

```
dev = "/dev/i2c-%d" % bus
fd = open(dev, O_RDWR)
ioctl(fd, 0x0703 /* I2C_SLAVE */, addr)
version = read16(REG_VERSION 0x08)
raw_v   = read16(REG_VCELL   0x02)
if raw_v in (0x0000, 0xFFFF): raise IOError("implausible VCELL read 0x%04X")
```
Any exception → close the fd if it was opened, print **three** lines and return
with `hardware = False`:
```
[BATT] HARDWARE NOT FOUND: Running in Simulation Mode ({exc}).
[BATT] This battery gauge is a stub for the QEMU dev environment.
[BATT] Simulated VCELL=3.85 V (override: NEODCT_BATT_SIM_VCELL env var or /tmp/neodct_sim_vcell).
```
On success keep the fd open **for the process lifetime**, set
`hardware = True`, `version = version`, and print two lines:
```
[BATT] MAX1704x fuel gauge @ 0x36 on /dev/i2c-3 (VERSION=0x%04X).
[BATT] Using REAL battery gauge: VCELL=%.3f V.
```

#### Register access (lines 131–143)

```c
/* write the register pointer, STOP, then a separate read transaction.
 * NOT a repeated-START combined transfer -- do not "improve" this into
 * an I2C_RDWR ioctl, the bus timing on the real board differs. */
static int read16(int fd, uint8_t reg, uint16_t *out) {
    uint8_t r = reg, d[2];
    if (write(fd, &r, 1) != 1) return -1;
    if (read(fd, d, 2) != 2)   return -1;
    *out = (uint16_t)((d[0] << 8) | d[1]);
    return 0;
}
static int write16(int fd, uint8_t reg, uint16_t val) {
    uint8_t b[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return write(fd, b, 3) == 3 ? 0 : -1;
}
#define SIGNED16(v) ((int32_t)((v) & 0x8000 ? (int32_t)(v) - 0x10000 : (int32_t)(v)))
```

#### `_read_vcell()` (line 145)

Simulation: `_read_vcell_sim()` → try `float(open("/tmp/neodct_sim_vcell").read().strip())`;
on any failure try `float(os.environ["NEODCT_BATT_SIM_VCELL"])`; on `KeyError`
or `ValueError` return `3.85`.

Hardware: `read16(REG_VCELL)`; `0x0000`/`0xFFFF` treated as an error. On error
`_read_error_streak += 1` and **only when the streak is exactly 1** print
`[BATT] VCELL read failed: {exc}`; return `None`. On a good read after a
non-zero streak print `[BATT] VCELL reads recovered after %d failures.` and
reset the streak. Return `raw * 78.125e-6`.

#### `poll(force=False)` → `"shutdown"` or `None` (line 175)

```
now = monotonic()
if not force and (now - _last_poll) < 2.0: return None
_last_poll = now

vcell = _read_vcell()
if vcell is None: return None                    /* keep the last known state */

_samples.append(vcell)                           /* deque maxlen 5 */
v = mean(_samples); _smoothed = v; _level = _level_for(v)

/* recovery re-arms the one-shots and drops a stale pending warning */
if !_low_armed  and v > 3.45 + 0.05:  _low_armed  = true;
                                      if _pending_warning == "low":      _pending_warning = NULL
if !_crit_armed and v > 3.25 + 0.05:  _crit_armed = true;
                                      if _pending_warning == "critical": _pending_warning = NULL

if v <= 3.20:
        _shutdown_count += 1
        if _shutdown_count >= 3: return "shutdown"     /* returns BEFORE latching */
else:   _shutdown_count = 0

if v <= 3.25:
        if _crit_armed:
                _crit_armed = false
                _low_armed  = false        /* don't follow up with the milder one */
                _pending_warning = "critical"
elif v <= 3.45:
        if _low_armed:
                _low_armed = false
                if _pending_warning != "critical": _pending_warning = "low"
return None
```

`_level_for(v)` = count of `{3.35, 3.55, 3.75, 3.95}` that are `<= v` → 0..4,
mapping to `bat-0.png` … `bat-4.png` in `ui_home.json`.

The mean is `sum(deque) / len(deque)` in IEEE-754 double, over **at most five**
samples, in insertion order. Use `double` and sum in the same order; a running
average would drift from the Python by a ULP and could flip a threshold.

#### `take_pending_warning()` / `level()` / `vcell()`

`take_pending_warning()` returns and clears the latch. `level()` returns the
cached 0–4. `vcell()` returns `_smoothed` (may be `None`).

#### `debug_snapshot()` (line 250)

`None` in Simulation Mode. Otherwise a dict with `bus, addr, version, level,
smoothed_v`, then a fresh read of `VCELL(0x02)`, `SOC(0x04)`, `CRATE(0x16)`,
`CONFIG(0x0C)` — **four reads, in that order**. Any exception adds
`"error": str(exc)` and returns early with only the first five keys. On success
it also carries `raw_vcell`, `vcell = raw*78.125e-6`, `raw_soc`,
`soc = raw_soc / 256.0`, `raw_crate`,
`crate = None if raw_crate == 0xFFFF else SIGNED16(raw_crate) * 0.208`, and
`config`.

#### `quickstart()` (line 285)

Hardware only (`False` in Simulation Mode). `write16(REG_MODE 0x06, 0x4000)`;
exception → print `[BATT] Quick-start failed: {exc}` and return `False`.

#### How the warnings and the shutdown reach the screen (`main.py`)

* `NeoDCT_UI._battery_tick()` (`main.py:968`) is the first thing
  `read_keypress()` does. It skips entirely once `_shutting_down` is set;
  wraps `battery.poll()` in a try that prints `[BATT] Poll failed: {exc}`; and
  calls `_shutdown_low_battery()` on `"shutdown"`.
* `_shutdown_low_battery()` (`main.py:1158`): set `_shutting_down`, print
  `[BATT] Battery empty (VCELL={v:.3f} V). Graceful shutdown.`, draw (without
  waiting for a key) `show_error(ui, "Battery empty. Shutting down...",
  title="LOW BATTERY", button_text=None, wait_for_ack=False)`, `sleep(3)`,
  `os.sync()`, `os.system("poweroff")`. **If `poweroff` returns non-zero**,
  print `[BATT] poweroff failed (rc={rc}); resuming so dev sessions survive.`,
  clear `_shutting_down` and return. Otherwise loop `sleep(1)` forever so the
  notice stays on screen while init takes the phone down.
* `show_pending_battery_warning()` (`main.py:1178`) is called from the **core
  loop only**, and returns immediately unless `state` is `"HOME"` or
  `"HOME_DIALING"` — so a latched warning waits until the user is back on the
  home screen and never lands mid-frame inside an app. Message text is
  `"BATTERY CRITICALLY LOW!"` or `"LOW BATTERY!"`, dialog title `"Battery"`,
  and it prints `[BATT] Warning: {message} (VCELL={v:.3f} V)`.

---

### 14. How the four are wired into the tick (`main.py`)

Construction order in `NeoDCT_UI.__init__` (`main.py:517`), before any input or
framebuffer setup: `init_databases()`, then `ModemService()`, `BatteryService()`,
`NotifyService()`, then `self._unread_sms = self._count_unread_sms()`.

`read_keypress(timeout=0.1)` (`main.py:1191`) begins with exactly three calls,
in this order, **before** any input is read:

```python
self._battery_tick()
self._modem_tick()
self._ring_tick()
```

`_modem_tick()` (`main.py:985`): `modem.poll()` inside a try that prints
`[MODEM] Poll failed: {exc}` **and returns**, then drains the event queue:
```
while True:
    event = modem.take_pending_event()
    if event is None: break
    try:
        if not self._handle_modem_event(event): break   /* busy: requeued */
    except Exception as exc:
        print "[MODEM] Event {kind} failed: {exc}"      /* and keep draining */
```

`_handle_modem_event(event)` (`main.py:1005`) handles only three kinds; every
other kind (`incoming`, `connected`, `ended`, `missed`, `sms_sent`,
`modem_lost`, `modem_found`) falls through and returns `True`, i.e. is
**discarded**:

| kind | action |
| --- | --- |
| `sms_received` | `fetch_sms(index)`; `"busy"` → requeue and return `False`; `"ok"` → insert into `sms_inbox.db`, `notify.post_sms(row_id)`, `_unread_sms += 1` |
| `sms_stored_check` | `read_stored_sms()`; `"busy"` → requeue and return `False`; for each record insert + `notify.post_sms(row_id, tone=(i == 0))` (**one beep, not N**) + `_unread_sms += 1` |
| `sms_sim` | insert `(sender, body)` + `post_sms` + `_unread_sms += 1` |

`_ring_tick()` (`main.py:1055`): returns if `_handling_call`; if
`modem.state != "RINGING"` clears `_ring_seen_at` and returns; on the first
RINGING tick stamps `_ring_seen_at = monotonic()`; **if `caller_id` is still
`None` and less than 0.60 s has passed, returns without raising** — a
deliberate one-poll grace so the incoming screen opens with the name rather
than "Unknown"; otherwise raises `IncomingCall(modem.caller_id)`.

`IncomingCall` derives from `BaseException`, not `Exception`, precisely so an
app's own `except Exception` cannot swallow a ringing phone. In the C design
this becomes SIGTERM to the app child (see `ARCHITECTURE.md`).

---

## Public interface (the functions other parts call)

Everything below is reached today as `ui.modem.*`, `ui.battery.*`,
`ui.notify.*`. In the process-per-app design these are the calls that must
cross the core↔app boundary; nothing else in these four services is public.

### Modem

```c
/* --- called from the UI/core thread ---------------------------------- */
bool        nd_modem_dial(nd_modem *m, const char *number);
bool        nd_modem_answer(nd_modem *m);
bool        nd_modem_hangup(nd_modem *m);
/* (ok, detail) -- detail is a short static/owned string, see below */
bool        nd_modem_send_sms(nd_modem *m, const char *number, const char *text,
                              char *detail, size_t detail_sz);
nd_sms_st   nd_modem_fetch_sms(nd_modem *m, int index, nd_sms_rec *out);       /* OK/BUSY/ERROR */
nd_sms_st   nd_modem_read_stored_sms(nd_modem *m, nd_sms_rec *out, size_t max,
                                     size_t *n_out);
void        nd_modem_poll(nd_modem *m);                 /* the tick          */
bool        nd_modem_take_pending_event(nd_modem *m, nd_modem_event *out);
void        nd_modem_requeue_event(nd_modem *m, const nd_modem_event *e);
void        nd_modem_status_snapshot(nd_modem *m, nd_modem_status *out);
/* raw AT passthrough for the engineering Modem app; default timeout 5.0 s */
nd_err      nd_modem_send_at(nd_modem *m, const char *cmd, double timeout,
                             char *final_out, size_t final_sz,
                             nd_strlist *lines_out);
void        nd_modem_close(nd_modem *m);

/* --- readouts polled every rendered frame ---------------------------- */
int         nd_modem_signal_level(nd_modem *m);   /* 0..4, or -1 for "None" */
const char *nd_modem_operator_display(nd_modem *m); /* NULL == "No Service" */
bool        nd_modem_registered(nd_modem *m);
void        nd_modem_call_status(nd_modem *m, const char **label, int *secs);
                                                  /* *secs < 0 == "None"    */

/* --- fields read directly today (must stay readable) ----------------- */
/*   m->state       "IDLE" | "CALLING" | "RINGING" | "CONNECTED"          */
/*   m->caller_id   last +CLIP number, or NULL                            */
/*   m->hardware    bool                                                  */
/*   m->imei        char[16+1] or empty                                   */
/*   m->port        char[N] or empty                                      */
```

`signal_level()` returning `None` is load-bearing: `render_element` falls back
to the layout's `sim_val` (default 3) when it is `None`, but uses `0` when it is
`0`. Encode "None" distinctly (`-1` above), do not conflate it with zero.

Call sites of the modem surface:

| Caller | Uses |
| --- | --- |
| `core/main.py` | `poll`, `take_pending_event`, `requeue_event`, `fetch_sms`, `read_stored_sms`, `state`, `caller_id`, `dial`, `answer`, `hangup`, `signal_level`, `operator_display` |
| `ui/Dialer/call_screen.py` | `call_status()`, `state`, `hangup()` |
| `ui/Dialer/incoming_screen.py` | `caller_id`, `state` |
| `apps/Messages/main.py` | `send_sms(number, text)` → `(ok, detail)`; detail is rendered verbatim in `"Send failed: %s"` |
| `engineering/apps/Modem/main.py` | `status_snapshot()`, `hardware`, `imei`, `send_at()` for `AT+CPIN?`, `AT+CNUM`, `AT+CICCID`, `AT+CCID`, `AT+CIMI`, `AT+CGMR` (all `timeout=3.0`) |

### Notify

```c
void        nd_notify_post_sms(nd_notify *n, int64_t row_id, bool tone);
bool        nd_notify_active(const nd_notify *n);
const char *nd_notify_kind(const nd_notify *n);          /* "sms" or NULL   */
int         nd_notify_count(const nd_notify *n);
int64_t     nd_notify_latest_data(const nd_notify *n);   /* -1 for None     */
/* two lines, or zero lines; buffers owned by the caller */
size_t      nd_notify_banner_lines(const nd_notify *n, char l1[32], char l2[32]);
void        nd_notify_dismiss(nd_notify *n);
bool        nd_notify_play_tone(nd_notify *n, const char *path);
bool        nd_notify_start_ring(nd_notify *n);
void        nd_notify_stop_ring(nd_notify *n);
bool        nd_notify_ringing(const nd_notify *n);
const char *nd_notify_ringtone_path(nd_notify *n);       /* NULL if none    */
```

### Battery

```c
nd_err      nd_battery_init(nd_battery *b, int bus, int addr);  /* -1,-1 = from settings */
const char *nd_battery_poll(nd_battery *b, bool force);  /* "shutdown" or NULL */
int         nd_battery_level(const nd_battery *b);       /* 0..4, starts at 3  */
bool        nd_battery_vcell(const nd_battery *b, double *out); /* false = None */
const char *nd_battery_take_pending_warning(nd_battery *b); /* "low"|"critical"|NULL */
bool        nd_battery_debug_snapshot(nd_battery *b, nd_battery_snap *out); /* false in sim */
bool        nd_battery_quickstart(nd_battery *b);
void        nd_battery_close(nd_battery *b);
/*   b->hardware   bool -- read by render_element to decide the "?" label
 *                 and by the FuelGauge app to refuse to run              */
```

### Clock

```c
/* all free functions, no object */
bool   nd_clock_build_epoch(time_t *out);
bool   nd_clock_last_known(time_t *out);
bool   nd_clock_remember(time_t when);
bool   nd_clock_set(time_t when, const char *reason);
bool   nd_clock_apply_floor(time_t *settled);            /* false == left alone */
nd_err nd_clock_query(const char *server, int timeout_s, time_t *out);
bool   nd_clock_sync(const char *const *servers, size_t n, int timeout_s, time_t *out);
void   nd_clock_start(bool background, const char *const *servers, size_t n);
bool   nd_clock_has_route(void);
```

---

## External dependencies and their C replacements

| Dependency | Used for | C replacement |
| --- | --- | --- |
| `termios` (`tcgetattr`/`tcsetattr`, `B115200`, `CS8`, `CREAD`, `CLOCAL`, `VMIN`, `VTIME`) | AT port and PCM port setup | Identical POSIX calls; `<termios.h>`. Zero new code. |
| `os.open/read/write/close` with `O_NONBLOCK` | raw serial I/O | `open`/`read`/`write`/`close`. Watch `EAGAIN` vs `EWOULDBLOCK` (same value on Linux). |
| `fcntl.flock(LOCK_EX|LOCK_NB)` on `/tmp/neodct-modem.lock` | serialising with `S45modem` and `atcmd` | `flock(2)` — **must be `flock`, not `fcntl(F_SETLK)`**; busybox `flock` in `atcmd` uses `flock(2)` and the two lock families do not interact. |
| `fcntl.ioctl(fd, 0x0703, addr)` | I²C slave address | `ioctl(fd, I2C_SLAVE, addr)` from `<linux/i2c-dev.h>`; the numeric value `0x0703` is the same. |
| `subprocess.Popen([...], DEVNULL, DEVNULL)` × 4 sites (`aplay` tone, `aplay` speaker, `arecord` mic, `mpv` ring) | fire-and-forget child processes | One shared helper: `posix_spawn` **or** `fork()`+immediate `execvp()` per CODING-STANDARDS §1.1. Redirect fds 1 and 2 to `/dev/null` in the child. Needs an explicit `waitpid(-1, …, WNOHANG)` reaper — see below. |
| `subprocess.call(["date","-s","@N"])` and `(["hwclock","-w"])` | setting the clock | `clock_settime(CLOCK_REALTIME, …)` for the system clock and `ioctl(fd, RTC_SET_TIME, &tm)` on `/dev/rtc0` for the RTC, **or** keep the two `execvp`s for a strict 1:1. See [Risks](#risks) — this is the one place I recommend a deviation. |
| `os.system("poweroff")` (in `main.py`, triggered by us) | graceful shutdown | Owned by `spec-core-loop.md`; needs `fork`+`execvp("poweroff")`+`waitpid` to reproduce the "rc != 0 → resume" branch. |
| `socket` (AF_INET/SOCK_DGRAM, `settimeout`) | SNTP | `socket(AF_INET, SOCK_DGRAM, 0)` + `SO_RCVTIMEO`/`SO_SNDTIMEO` set to 5 s, or `poll()` around the recv. Hostname resolution via `getaddrinfo`. |
| `struct.unpack("!I", …)` | NTP transmit timestamp | Explicit byte assembly, **not** a cast: `(b[40]<<24)|(b[41]<<16)|(b[42]<<8)|b[43]` — CODING-STANDARDS §6 bans endianness assumptions. |
| `threading.Thread(daemon=True)` | the clock-sync worker | one `pthread_create` + `pthread_detach`. |
| `collections.deque(maxlen=8)` | the modem event queue | fixed 8-slot ring with the drop-oldest / drop-newest semantics described above. No allocation. |
| `time.monotonic()` | every timer in all four services | `clock_gettime(CLOCK_MONOTONIC)` → `double` seconds, or fixed-point ms. **Never** `CLOCK_REALTIME`: `ClockService` moves the wall clock under the modem's and the battery's feet at boot. |
| `time.time()` / `time.gmtime` / `time.strftime` | clock logs, envelope blink | `time(NULL)`, `gmtime_r`, `strftime`. The envelope blink genuinely uses wall-clock — keep it. |
| **`miniaudio`** (`decode_file`, `PlaybackDevice`) | the looping ringtone | The only real port. See below. |
| **`aplay` / `arecord`** (BusyBox-adjacent, from `alsa-utils`) | tone playback, call audio | Stay as external binaries. Both defconfigs set `BR2_PACKAGE_ALSA_UTILS_APLAY=y`, which installs both. **No change.** |
| **`mpv`** | ringtone fallback | Both defconfigs set `BR2_PACKAGE_MPV=y`. Keep the fallback path invoking the same binary. |
| `os.listdir` + `/sys/class/tty/*/device/../bInterfaceNumber` | AT/PCM port discovery | `opendir`/`readdir` + collect + `qsort` with `strcmp` (readdir is unordered; Python's `listdir` is too, but the code always `sorted()`s it — do the same). |
| `/proc/asound/cardN/pcm*c` | ALSA capture-device discovery | Same directory walk, same `strcmp` sort. |
| `/proc/net/route`, `/proc/net/ipv6_route` | default-route check | `fopen`/`fgets`/`strtok_r`. |
| `SettingsStorage.get_setting` | 8 keys, above | `libneodct.so`'s settings API (owned by another survey). **Note it is not cheap**: each call reads `settings.prop` *and* `version.prop` and may rewrite `settings.prop`. The two per-call-audio sites and the per-ring site call it on a user-visible path. |

### Replacing miniaudio — the ringer

`miniaudio` is a Python binding around the `miniaudio.h` single-header C
library. The C port does not need the binding; it can use the same header, or a
smaller pair. What the Python asks for is narrow:

* decode a whole file to **signed 16-bit, 2 channels, 44100 Hz**;
* open a playback device at the same format with a **500 ms** buffer, named
  `"NeoDCT Ring"`;
* pull `required` frames per callback from a wrap-around cursor over the
  decoded buffer.

Formats that must actually decode: **MP3** (all 16 shipped ringtones) and
**WAV** (`sms.wav` is the last fallback). The extension-retry list also names
`.wma`, `.flac` and `.ogg`, but those only affect *path selection* — miniaudio
cannot decode WMA at all, so a `.wma` tone already falls through to the mpv
branch today. Reproduce that: attempt the decode, fail, log
`[NOTIFY] miniaudio ring failed (…); trying mpv.`, spawn mpv.

Recommended C stack: `dr_mp3` (or `minimp3`) + `dr_wav`, both single-header and
both already inside `miniaudio.h`; ALSA `libasound` for output, or reuse
`miniaudio.h` itself in `MA_NO_DECODING`-off mode. Roughly 300 lines of glue.

**The measured problem with decoding the whole file.** Every shipped ringtone is
64 kbps mono, mostly 48 kHz. Decoded to 44100 Hz *stereo* int16, as the Python
does, they cost:

| Tone | Duration | Decoded buffer |
| --- | --- | --- |
| `Low.mp3` (the default) | ~3.5 s | **599 KB** |
| `Nokia Tune.mp3` | ~4.8 s | 828 KB |
| `Ring Ring.mp3` | ~7.2 s | 1.2 MB |
| `Bumblebee.mp3` | ~25.2 s | 4.3 MB |
| `Valkyrie.mp3` | ~31.5 s | 5.4 MB |
| `Tchaikovsky.mp3` | ~36.3 s | **6.3 MB** |
| `Brave Scotland.mp3` | ~36.1 s | **6.2 MB** |

On a target of "the whole OS under 8 MB", a user who picks Tchaikovsky as their
ringtone adds 6.3 MB to the **core** process the moment the phone rings, and
holds it until they answer. That single allocation is bigger than the entire
memory budget for everything else. This is the largest memory finding in this
subsystem and it is flagged in [Risks](#risks) with the proposed fix (streaming
decode with a small ring buffer, audibly identical, ~64 KB).

### Process reaping

Four `Popen` sites plus (in `main.py`) app children and `poweroff`. Python
reaps opportunistically; C will not. The core needs one `SIGCHLD` handler
doing:

```c
/* async-signal-safe: waitpid only, no logging from the handler */
while (waitpid(-1, &status, WNOHANG) > 0)
    ;   /* app-child status is recovered by the core's own blocking waitpid */
```
…but that races with the core's `waitpid(app_pid)`. The clean shape is a single
child registry in the core: every spawn registers its pid and an owner tag
(`ND_CHILD_APP`, `ND_CHILD_AUDIO`, `ND_CHILD_TONE`), a `signalfd`/self-pipe
delivers `SIGCHLD` into the core's poll set, and one reaper routes exit statuses
to the right owner. `_watch_audio_proc` needs the *exit code* of the aplay and
arecord children for its log lines, so a blind `SIG_IGN` is not acceptable.

---

## Proposed C modules

| File | Contents | Est. LOC |
| --- | --- | --- |
| `core/nd_modem.h` | `nd_modem`, `nd_modem_event`, `nd_modem_status`, `nd_sms_rec`, all constants above | 140 |
| `core/nd_modem_at.c` | flock helpers, `open_port` termios, `read_pending` line splitter with a bounded rx buffer, `transact`, `command`, `drop_hardware` | 320 |
| `core/nd_modem.c` | construction, port probe/candidate enumeration, `init_modem` command list, URC dispatch, `parse_reg`/`parse_csq`/`parse_cops`/`poll_clcc`, `poll()` scheduler, event ring, dial/answer/hangup, SMS send/fetch/sweep, `parse_sms_records`, readouts, `status_snapshot` | 760 |
| `core/nd_modem_audio.c` | PCM port discovery, PCM termios, ALSA capture scan, speaker/mic spawn, `watch_audio_proc`, `stop_call_audio` | 260 |
| `core/nd_modem_sim.c` | the four `/tmp/neodct_sim_*` hooks and the re-probe timer | 140 |
| `core/nd_notify.h` | `nd_notify`, banner constants, ring fallback tables | 60 |
| `core/nd_notify.c` | banner state, `play_tone`, `ringtone_path` resolution chain | 190 |
| `core/nd_ringer.c` | decode + ALSA playback + wrap-around cursor + mpv fallback | 300 |
| `core/nd_clock.h` / `nd_clock.c` | prop reader, state file with atomic rename, `set_clock`, `apply_floor`, SNTP `query`/`sync`, route check, worker thread | 300 |
| `core/nd_battery.h` / `nd_battery.c` | i2c open + `I2C_SLAVE`, `read16`/`write16`, sim source, smoothing ring, warning latches, `debug_snapshot`, `quickstart` | 330 |
| `core/nd_child.c` / `nd_child.h` | shared spawn (`fork`+`execvp`, fds to `/dev/null`) and the SIGCHLD reaper/registry — **coordinate with `spec-core-loop.md`, which also needs this for app children and `poweroff`** | 180 |
| `tests/unit/test_modem_parse.c` | URC/CSQ/COPS/CLCC/CMGR/CMGL parsers against captured strings | 220 |
| `tests/unit/test_battery.c` | level thresholds, smoothing, latch/re-arm/shutdown-confirm state machine | 150 |
| `tests/unit/test_clock.c` | port of `test_clockservice.py`, all 14 cases | 200 |
| **Total** | | **≈ 3550** (≈ 3050 excluding tests) |

The split between `nd_modem_at.c` and `nd_modem.c` is deliberate: the AT engine
has no knowledge of calls or SMS, which makes it the piece that can be unit
tested against a pty without a modem.

---

## Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| **Ringtone decode is up to 6.3 MB in the core process.** `miniaudio.decode_file` materialises the whole tone as 44.1 kHz stereo int16; the shipped tones run to 36 s. On an 8 MB target this single allocation can exceed the whole budget. | **high** | Stream instead of decoding whole: `dr_mp3`/`dr_wav` pull-decoder feeding a ~64 KB ring buffer, seeking back to frame 0 at EOF. Audibly identical (the Python loop is already sample-exact with no gap). Deviation from a literal 1:1 — record it in `OPEN-QUESTIONS.md` and in the module comment. Cap: refuse to ring from a file over N MB and fall back to `Low.mp3`. |
| **Unbounded `_rxbuf`.** `_read_pending` appends every 512-byte chunk and only ever removes complete lines. A modem stuck emitting binary with no `\n` (the DIAG port is exactly this, and `iface == None` candidates can reach it) grows the buffer without limit. Python hides it; C will OOM or, worse, `realloc`-thrash. | **high** | Hard-cap the rx buffer at e.g. 8 KB; on overflow discard the buffer, log once, and continue. Behaviour is unchanged for any real AT session. Raise as an open question — this is a latent bug in the Python too. |
| **The modem moves from "polled inside `read_keypress`" to its own thread** (`ARCHITECTURE.md`). Every field the UI reads (`state`, `caller_id`, `operator`, `_csq`, `_reg_stat`, event ring) becomes shared, and `dial`/`answer`/`hangup`/`send_sms`/`fetch_sms`/`send_at` are all called from the other thread while holding the port. | **high** | One modem thread owns the fd and the parsed state. UI-thread entry points post a request onto a small queue and block on a condvar; the modem thread services it between URC drains. Snapshot readouts (`signal_level`, `operator_display`, `call_status`, `state`) take a short mutex and copy. Do **not** let two threads touch the fd. |
| **`send_sms` blocks for up to 35 s while holding the port** (5 s prompt + 30 s ack), and `dial` for up to 8 s. Today that blocks the whole UI, which is at least honest. On a thread it must not block URC handling. | medium | The same request-queue design: the SMS state machine runs on the modem thread and keeps draining URCs between polls, exactly as `_wait_sms_prompt` and the ack loop already do (both call `_handle_urc`). Preserve those calls. |
| **Four `Popen`s with no reaping.** DTMF fires one per keypress. Without a reaper the core accumulates zombies until it hits `RLIMIT_NPROC`. | medium | Central child registry + `SIGCHLD` via `signalfd`/self-pipe, described above. Must be built before the first tone plays. |
| **`set_clock` shells out to `date -s` and `hwclock -w` from a process that will have threads.** Fork-without-exec in a threaded process is the exact trap CODING-STANDARDS §1.1 bans, and the clock worker *is* a thread. | medium | Use `clock_settime(CLOCK_REALTIME)` + `ioctl(RTC_SET_TIME)` on `/dev/rtc0`. Non-observable except that it no longer depends on busybox `date`. If a strict 1:1 is wanted, `posix_spawn` is safe (it does the exec itself). Log line must stay byte-identical either way. |
| **Double `close()` in `_probe_ports`.** Python swallows the second close; C will close a *different* fd if the number has been recycled by another thread. | medium | Track `fd_owned` and only close once. External behaviour unchanged. |
| **`os.system("poweroff")` return-code branch.** `os.system` returns a wait status, not an exit code, so `rc != 0` is true whenever `poweroff` is killed by a signal *or* exits non-zero. | low | Reproduce with `waitpid` + the same "non-zero wait status resumes" test, not `WEXITSTATUS`. Note it in the module comment. |
| **`AF_INET`-only SNTP on an IPv6-only bearer.** `S45modem` brings up an IPv6-only NAT64/DNS64 T-Mobile bearer. `query()` creates an `AF_INET` socket, so on a v6-only route the sync silently fails on all three servers and the phone keeps the build date. | low | This is existing behaviour, and the tests do not cover it. Port as-is, note it in `OPEN-QUESTIONS.md` — switching to `getaddrinfo` with `AF_UNSPEC` would be a one-line behaviour change but *is* a behaviour change. |
| **Floating-point drift in the battery smoother.** A running average or a different summation order can flip a reading across 3.20/3.25/3.45 V and change whether the phone powers off. | low | Sum the ≤5 samples in insertion order, in `double`, exactly as `sum(deque)/len(deque)` does. Compare against the Python on the same input vector in a unit test. |
| **`plughw:` / port discovery ordering.** Both use byte-wise sorts where a human would expect numeric order (`ttyUSB10` before `ttyUSB2`, `card10` before `card2`). | low | Use `strcmp` in `qsort`. Do not "fix" it. |
| **`get_setting` cost on the ring/call path.** Each call re-reads two files and may rewrite `settings.prop`, on a path the user can feel (dialling, ringing). | low | Out of scope here — flagged to the settings survey. Do not cache it in these modules; that would change when a setting takes effect. |
| **Simulation-mode file stats at 10 Hz.** `_poll_sim` runs unthrottled and `signal_level`/`operator_display` `open()` a `/tmp` file per rendered frame. | low | Keep it (it is how the hooks stay live) but budget the syscalls; on the real device `hardware` is true and none of this path runs. |

---

## Tests that cover this

| Test file | Covers | Usable as a port oracle? |
| --- | --- | --- |
| `neodct/tests/test_clockservice.py` (14 tests) | **ClockService, thoroughly.** The offline floor (epoch → build date; a clock already ahead is left alone; a remembered sync beats the build date; a corrupt state file is not fatal), the SNTP wire format (`_reply()` builds a real 48-byte packet with the timestamp at offset 40), rejection of pre-2020, zero and truncated replies, the server walk (`sync` moves on when one is silent, returns `None` when all are), `remember()` round-tripping, that the servers are the volunteer pool only, and that `set_clock` logs `[CLOCK]`, `setting time` and the reason. | **Yes — port it case for case.** Every assertion is on values the C will produce identically. `tests/unit/test_clock.c` should be a direct transcription. |
| `neodct/tests/test_uistub.py::test_simulate_status_presents_a_registered_device_with_a_battery` | Sets `ui.battery.hardware = True`, `ui.battery._level = 4`, and monkeypatches `ui.modem.signal_level` / `operator_display`, then asserts the home screen renders the bars, the gauge and the carrier text. Also the negative case (`battery=2, signal=0`, no carrier → `operator_display() is None`). | **Yes for the rendering contract** — it pins that `signal_level()` and `operator_display()` are the only two hooks the home screen consults, and that `hardware=False` must draw `?`. Not a test of the services themselves. |
| `neodct/tests/golden/manifest.json` + `neodct/tools/uistub.py` | 240×175 SHA-256 golden frames of every screen, captured from the real Python UI. The home-screen frames encode the banner position, the envelope blink sprite, the signal bars and the battery gauge. | **Yes, and this is the important one.** The C build must reproduce these hashes. Note the envelope blink is wall-clock phased (`int(time.time()*2) % 2`), so the harness must pin the clock (the manifest already records `"epoch": 1704112496.0`). |
| — | **ModemService: no tests at all.** | The 1084-line file with the AT state machine, the SMS parser and the call flow is completely uncovered. |
| — | **NotifyService: no tests at all.** | Uncovered. |
| — | **BatteryService: no direct tests.** Only reached indirectly via `uistub`'s `hardware`/`_level` poking. | The warning-latch state machine, the smoothing window and the shutdown confirmation are uncovered. |

**What to build before writing the C.** Three of the four services have no
oracle, and the parsers are exactly the kind of code that a port gets subtly
wrong. Cheap, high-value additions that can be written against the *Python*
first and then reused as the C test vectors:

1. A corpus of real AT transcripts — `+CSQ:`, `+COPS:` with and without quotes,
   `+CEREG:` in both query and unsolicited forms, `+CLIP:`, `+CMTI:`, `+CLCC:`,
   a full `+CMGR:` and a multi-record `+CMGL:` — with the expected parsed
   output. Feed them to both `_parse_*`/`_handle_urc` and the C equivalents.
2. A pty-backed fake modem that replays a scripted session, so `_transact`,
   `_wait_sms_prompt` and the whole `send_sms` flow can be exercised on the
   host in both languages.
3. A voltage-vector test for `BatteryService.poll`: a list of VCELL readings
   and the expected `(level, pending_warning, shutdown)` after each, covering
   the re-arm hysteresis and the 3-sample shutdown confirmation.

---

## How this could be split across agents

The four services share almost nothing — no data structures, no headers beyond
`nd_log` and the settings API. The only genuine coupling is the shared child
spawn/reap helper. So this is very parallelisable, with one sequencing rule.

**Sequencing rule:** `nd_child.c` (spawn + SIGCHLD registry) must land before
the modem-audio and notify agents can finish, and it is also needed by the core
loop for app children. Have the core-loop agent own it and publish the header
early, or give it to one of the agents below on day one as a 180-line
standalone task.

| Agent | Scope | Depends on | Notes |
| --- | --- | --- | --- |
| **A — AT engine** | `nd_modem_at.c` + `nd_modem.h`. Termios, flock, bounded line reader, `transact`, `command`, `drop_hardware`. Ships with the pty-backed fake modem and its unit tests. | settings API only | Genuinely standalone. Start here — everything else in the modem depends on it and nothing depends on the rest. |
| **B — modem state machine** | `nd_modem.c`: probe, `init_modem`, URC dispatch, all parsers, the `poll()` scheduler, dial/answer/hangup, SMS. | A | The biggest single piece (~760 LOC). Could be split again at the SMS boundary (parsers + `send_sms`/`fetch_sms`/`read_stored_sms` is a clean ~250-line slice) if two agents are available. |
| **C — modem audio + simulation** | `nd_modem_audio.c`, `nd_modem_sim.c`. | A (for the struct), `nd_child` | Touches no parsing. The sim hooks are pure file I/O and can be written and tested with no modem at all. |
| **D — notify + ringer** | `nd_notify.c`, `nd_ringer.c`. | `nd_child`, settings API | The ringer's streaming decoder is the one piece of real new engineering in this subsystem; give it to whoever is comfortable with ALSA. The banner half is trivial and could go with the core-loop agent instead, since the pixels live there. |
| **E — clock** | `nd_clock.c` + the transcription of `test_clockservice.py`. | nothing | Fully independent, has a complete test suite already written, and is the best first task for an agent finding its feet. ~300 LOC. |
| **F — battery** | `nd_battery.c` + the voltage-vector tests. | settings API | Fully independent. ~330 LOC. Pairs naturally with E for one agent. |

A reasonable four-agent plan: **A→B** as one agent's serial track (the critical
path, ~1100 LOC), **C** as a second, **D** as a third, **E+F** as a fourth.
The four converge only at `nd_core.c`, where the core loop constructs all of
them in the documented order (modem, battery, notify) and wires the tick.

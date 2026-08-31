# The app -> core service channel — design and specification

*Answers `OPEN-QUESTIONS.md` **MSG-1** ("Sending an SMS from an app process cannot reach
the modem"), and the half of **PB-5** / **DL-10** that says "an app process has
`ui->modem == NULL`, so it needs a route to the core that does not exist yet".*

Companion documents: `nd_app.h` (the process boundary and the inherited descriptors),
`nd_proc.h` (fork/exec and the app launcher), `spec-core-services.md` (what the modem and
the battery actually do), `SECURITY.md` (why the child is the less trusted end).

---

## 1. What this is, in plain English

In the Python, an app ran *inside* the core process — `exec_module()` dropped it straight
into the same interpreter — so `system.ui.modem` was simply there, a live object the app
could call. Sending a text was one method call.

In C an app is a **separate process**, so that a null dereference in an app kills the app
and not the phone. The price of that isolation is that the app can no longer reach into
the core's memory, and `nd_ui.h` says so:

```c
/* --- services, all owned by the core process --- */
nd_modem *modem;
nd_battery *battery;
nd_notify *notify;
```

`nd_ui_init_app()` leaves all three NULL. Until now nothing filled the gap its own comment
promised — *"those live in the core and an app that needs them asks across the boundary"* —
so three apps were broken:

| app | what the user saw |
| --- | --- |
| Messages | "ModemService is not running." instead of sending the text |
| Modem (eng.) | the same dialog, modem plugged in or not |
| FuelGauge (eng.) | "This tool needs the fuel gauge" with a gauge on the i2c bus |

This document specifies the asking mechanism: a **narrow, fixed request/response channel**
on a fourth inherited descriptor, served by the core, with exactly four operations on it.

---

## 2. Transport: one `AF_UNIX` `SOCK_SEQPACKET` socketpair per app launch

`nd_proc_launch_app()` already hands the child three descriptors whose numbers travel in
the environment (`nd_app.h`): `NEODCT_KEYPAD_FD`, `NEODCT_CRASH_FD`, `NEODCT_FB_FD`. A
fourth joins them:

```
NEODCT_SERVICE_FD   both ends of a socketpair: the app writes requests and
                    reads responses on it; the core does the reverse.
```

### Why a socketpair and not a third pipe

A pipe is one-way, so a request/response channel needs two of them — two descriptors, two
environment variables, two lifetimes to get right. One socketpair is one of each.

### Why `SOCK_SEQPACKET` and not `SOCK_STREAM`

**Framing.** A stream socket would need a length prefix and a re-assembly loop on both
ends, and the child is the untrusted end: a hostile or merely buggy app that writes half a
request and stops would leave the core's parser mid-message, and one that lies in its
length prefix would have the core reading a message that never ends. `SOCK_SEQPACKET`
preserves message boundaries in the kernel. One `recv()` returns exactly one record or
nothing, and a short write cannot desynchronise the next request. Both ends read into a
buffer **one byte longer than the record**, so a datagram that is too long comes back one
byte too long instead of being silently truncated into something that looks well formed;
anything whose length is not exactly `sizeof(record)` is refused before it is looked at.
The framing bug class simply does not exist.

The records are fixed-size PODs of fixed-width integers and `char` arrays. Both ends are
the **same `libneodct.so` build in the same process family on the same machine**, exactly
as `nd_input.h` argues for its own native `struct input_event` records, so there is no
endianness or padding question to answer. Every record still carries a magic number, a
protocol version and its own size, and the receiver checks all three plus the byte count
`recv()` reported: a mismatch is a protocol error that closes the channel, never a parse.

### Lifetime

The socketpair is created inside `nd_proc_launch_app()` immediately before the fork, listed
in the descriptor plan so it survives the exec, and **both ends die with the launch**. This
is deliberate and it is why there is no long-lived socket in the core:

- a stale request written by an app that has since died can never be read by the *next*
  app, because the next app gets a fresh pair;
- there is no drain-between-launches step to get wrong;
- `nd_proc.h` guarantees there is only ever one app child, so one pair per launch is also
  one pair at a time.

### Ordering around the fork

`CODING-STANDARDS.md` §1.1 is not weakened. The order is:

1. `socketpair()`, `pipe()`, `nd_input_channel_open()` — everything allocated **before**
   the fork;
2. `fork()` + `execve()`, with nothing but `dup2`/`fcntl` in between, as before;
3. the core closes its copy of the child's end;
4. **only then** the service thread is created.

The service thread therefore does not exist at the instant of the fork, and no new mutex
can be held by a thread the child inherits nothing of.

---

## 3. Blocking: the core answers on its own thread, and never on the pump loop

### The constraint that decides this

While an app runs, the core's UI thread sits in `nd_proc_launch_app()`'s pump loop:

```c
for (;;) {
    if (nd_proc_wait(pid, 0.0, &st) == ND_OK)
        break;
    pump_keys(ui, &ch);          /* nd_input_read_event(ui->input, 0.05, &ev) */
}
```

An SMS send takes up to `ND_SMS_PROMPT_TIMEOUT_S` + `ND_SMS_SEND_TIMEOUT_S` — call it
**37 seconds** in the worst case. Serving the request inline, on that loop, would stop
`pump_keys()` for those 37 seconds, and **on the real phone that loses keypresses rather
than delaying them**: `nd_input.c`'s `ND_INPUT_MATRIX` backend is scanned *synchronously
from inside `nd_input_read_event()`*. The i2c matrix has no kernel queue behind it. A key
pressed while nobody is scanning was never seen by anything and is gone. (Under evdev — a
desktop, QEMU — the kernel does buffer, so this failure is invisible on every machine a
developer is likely to test on. That is exactly the kind of divergence this port is
supposed to catch on paper.)

So: **the core serves requests on a dedicated thread**, created after the fork and torn
down after `waitpid()`. The pump loop is not touched at all, which also means this change
cannot regress key delivery, key repeat or held-key state.

### Why serving from another thread is safe

- `nd_modem.h` states it: *"Everything in this header is safe to call from the UI thread;
  the implementation takes the modem's own mutex."* `nd_modem_send_sms()` posts to the
  modem thread's request slot and waits on a condition variable; calling it from the
  service thread instead of the UI thread changes nothing.
- The battery has no lock — but it does not need one here. `OPEN-QUESTIONS.md` **X-13**
  records that *"while an app child runs, the core pumps keys but does not tick the
  services"*: `pump_keys()` calls `nd_input_read_event()` directly, not
  `nd_ui_read_keypress()`, so nothing else in the core touches `nd_battery` between the
  fork and the `waitpid()`. The service thread is the only toucher for exactly the window
  in which it exists. **This is a precondition, not a coincidence** — if the pump loop ever
  grows a battery tick, the battery needs a mutex on the same day.
- Nothing on this thread forks, draws, or writes to the canvas.

### Timeouts, and what happens at each failure

| where | limit | on expiry |
| --- | --- | --- |
| app waiting for a response to `SEND_SMS` | `ND_SVC_SMS_TIMEOUT_S` = **45 s** | the call returns "not sent" with detail `"no answer from the core"`; the channel is marked dead and every later call fails immediately |
| app waiting for any other response | `ND_SVC_TIMEOUT_S` = **5 s** | same, minus the wording |
| either end *writing* a record | `ND_SVC_TIMEOUT_S` = **5 s** | treated as a peer that has gone; see below |
| core waiting for a request | none — it polls with a 200 ms timeout and re-checks the stop flag | n/a |
| core stopping the thread after the app exits | `ND_SVC_JOIN_S` = **2 s** | the thread is detached and frees its own context when its in-flight request finishes; the core does not wait |

The **write** deadline is the least obvious of the four and it is not decoration. A reader
that has *gone* is easy — the send fails with `EPIPE`, and `MSG_NOSIGNAL` keeps that an
error rather than a death. The case that needs a deadline is a peer that is **alive and
not reading**: a hostile app can post request after request and never read an answer,
filling the core's socket buffer, and a blocking send would then park the service thread
there for ever — where it survives the 2 s join, gets detached, and never exits. One
untrusted child would leak a core thread per launch. Bounded, it cannot.

45 s is chosen to sit above the core's own worst case (2 s `AT+CMGF` + 5 s prompt + 30 s
ack ≈ 37 s) with margin, and to be finite: the point of a timeout here is that a *wedged
core* must not wedge the app, and a number below the core's honest worst case would abort
sends that were about to succeed.

In practice the app almost never reaches a timeout, because the socket tells it the truth
sooner: if the core dies, `recv()` returns 0 immediately and the app fails on the spot with
`"core service is gone"`. The timeout covers the remaining case — a core that is alive but
not answering.

The 2 s join deadline covers the reverse: an app killed (`SIGTERM` for an incoming call,
`SIGKILL` after the grace period) while a send is in flight on the service thread. There is
no way to abort a CMGS transaction that is already on the wire, and blocking the core for
up to 37 seconds to watch one finish would be the very freeze this design avoids. So the
thread is abandoned to finish and free itself, and the core gets its screen back at once.
This path is close to unreachable — an app waiting for a send is blocked in `recv()` and
cannot fault, and the modem thread is mid-transaction so it cannot parse a `RING` — but it
is written down and implemented rather than assumed away.

---

## 4. Scope of the API: four operations, and no more

> **Superseded in part by §9.** A fifth operation — taking the phone down — was added
> later, and §9 makes the argument this section demands of anything that joins the list.
> This section is deliberately left as it was written: it is the record of the reasoning at
> the time, and rewriting it to say "five" would erase the thing §9 has to answer to.

The channel exposes **exactly what the three broken call sites need** and nothing else.
This is the whole point of the exercise. `nd_modem.h` also offers `nd_modem_dial()`,
`nd_modem_answer()`, `nd_modem_hangup()`, `nd_modem_fetch_sms()`,
`nd_modem_read_stored_sms()` and raw `nd_modem_send_at()`. **None of them is on the wire.**
An app cannot dial, cannot answer, cannot hang up a live call, and cannot type an arbitrary
AT command at a modem that would happily accept `ATH`.

| op | request | response | serves |
| --- | --- | --- | --- |
| `ND_SVC_OP_SEND_SMS` | number, text | sent?, `detail` | `Messages/main.c` |
| `ND_SVC_OP_MODEM_STATUS` | — | `present?`, `nd_modem_status` | `Modem/main.c` |
| `ND_SVC_OP_BATTERY` | — | `present?`, `nd_battery_snap`, `hardware`, `level`, smoothed volts, `errno` | `FuelGauge/main.c` |
| `ND_SVC_OP_BATTERY_QUICKSTART` | — | ok? | `FuelGauge/main.c` |

### Two notes on the edges of that list

**Battery quick-start is included, deliberately.** It is the fourth op and it is a *write*,
so it deserves the argument. It is inside the cited call-site range (`FuelGauge/main.c:305`),
it is the only interactive control that app has, and leaving it dead would have been a
silent behaviour change from the Python rather than a port of it. It writes one register on
one i2c device — it cannot reach the modem, cannot touch a call, and cannot be used to
exfiltrate anything. `nd_battery_quickstart()` already refuses when there is no hardware.

**The Modem app's SIM page is deliberately *not* served.** That page is six raw AT
transactions (`AT+CPIN?`, `AT+CNUM`, `AT+CICCID`, `AT+CCID`, `AT+CIMI`, `AT+CGMR`), and
putting an app-chosen AT string on this wire is precisely the "expose the whole modem"
mistake. In an app process `sim_rows_present()` now runs against a NULL `nd_modem *`;
`nd_modem_send_at(NULL, …)` returns `ND_ERR_INVAL` and the app's own `transact()` already
turns that into `final == ""`, which is Python's `final is None` — the "no modem, or a
locked port" case the page was written to render. So it comes out `SIM "no reply"`,
`NUM "(not on SIM)"`, `ICCID`/`IMSI`/`FW` `"--"`, and **IMEI still real**, because that one
comes from the status snapshot beside them. Every one of those strings already existed for
a modem that would not answer, and that is exactly what happened: it was not asked.

If that page is later wanted in full, the shape is a **fifth op that runs a fixed list of
commands core-side and returns the parsed fields** — never a passthrough where the app
chooses the string. That keeps the surface narrow. It is not done here because it means
moving `sim_rows_present()`'s parser out of the app and into `lib/`, which is a different
work package.

---

## 5. Trust: what the core validates

The child is the less trusted end (`SECURITY.md`). Everything crossing the boundary
towards the core is validated before it reaches a service, and a failed check is answered
with `ND_SVC_ST_BAD_REQUEST` — it never silently rewrites the request into something
acceptable.

**The record itself**

- `recv()` must return exactly `sizeof(nd_svc_req)` bytes, with no `MSG_TRUNC`;
- `magic` is `ND_SVC_MAGIC`, `version` is `ND_SVC_VERSION`, `size` is `sizeof(nd_svc_req)`;
- `op` is one of the four.

**The phone number** — attacker-influenced text that ends up inside
`AT+CMGS="<number>"\r`. It is rejected, not filtered, unless it is:

- NUL-terminated inside `ND_MODEM_NUMBER_MAX`;
- 1 to 31 characters long;
- drawn only from `0123456789*#+`, the same set `nd_modem__filter_number()` keeps, with
  `+` permitted only as the first character.

`do_send_sms()` filters the number itself, so this is belt *and* braces — and that is the
point. The core must not be one refactor of a downstream filter away from letting an app
write `"\r\nATH\r\n` into the command stream.

**The message body**

- `text_len` < `ND_MODEM_TEXT_MAX`, and `text[text_len]` is NUL;
- no embedded NUL before `text_len`;
- well-formed UTF-8 (the composer only ever produces UTF-8; malformed input would be fed
  to `ascii_replace()`'s decoder, and a parser should not be handed junk it can be spared);
- not empty.

Newlines are **kept**, exactly as `do_send_sms()` keeps them: the body is sent after the
`>` prompt and terminated by Ctrl-Z, so it is data and not a command line. Ctrl-Z and ESC
continue to be stripped by the modem layer, unchanged, because that is what the Python did
and where it did it.

**Coming back the other way**, the app treats the response as untrusted too: every string
in it is re-terminated before use, and `detail` is bounded by `ND_MODEM_DETAIL_MAX` — the
width `nd_modem.h` already gives it, which is what makes Messages' `"Send failed: %s"`
buffer arithmetic still correct.

---

## 6. The API

`include/nd_svc.h`. The rule that keeps every golden frame safe is in the first line of
each function: **a direct handle always wins.**

```c
bool nd_svc_modem_present(nd_ui *ui);
bool nd_svc_send_sms(nd_ui *ui, const char *number, const char *text,
                     char *detail, size_t detail_sz);
bool nd_svc_modem_status(nd_ui *ui, nd_modem_status *out);

bool nd_svc_battery_present(nd_ui *ui);
bool nd_svc_battery_read(nd_ui *ui, nd_svc_battery *out);
bool nd_svc_battery_quickstart(nd_ui *ui);
```

Each one calls the service directly when `ui->modem` / `ui->battery` is non-NULL, and only
otherwise goes to the wire. In the core, and in `nd-shoot`'s in-process app runs — which is
how `eng-modem`, `eng-fuelgauge` and every other reference frame is captured — the handle
is non-NULL, the wire is never touched, and the executed code is byte-for-byte the code
that produced the references.

`nd_svc_battery_read()` bundles what FuelGauge used to get from three separate calls
(`nd_battery_debug_snapshot()`, `nd_battery_has_hardware()`, `nd_battery_vcell()`) into
**one** round trip. That is not just cheaper: three round trips would be three different
instants, and the app renders them on one frame. It carries the `errno` the snapshot left
behind explicitly, because `errno` does not cross a socket and `FuelGauge` renders
`strerror()` of it.

The client half is process-global, opened by `nd_ui_init_app()` from `NEODCT_SERVICE_FD`
and closed by `nd_ui_teardown()`. It follows the precedent `nd_ui.c` already sets for
`g_sim` and `g_ring_seen_at`: *"there is exactly one `nd_ui` per process, so instance state
with nowhere to live in the frozen struct lives here."* No field is added to `nd_ui`.

---

## 7. What each of the three apps does now

**Messages** (`nd_msg_send_flow`). The `ui->modem == NULL` test becomes
`!nd_svc_modem_present(ui)`, and `nd_modem_send_sms()` becomes `nd_svc_send_sms()`. Both
dialogs are untouched, and so is every string:

- no route, or a core with no `ModemService` -> **"ModemService is not running."**
- sent -> **"Message sent!"**
- refused -> **"Send failed: `<detail>`"**, where `<detail>` is the modem's own wording
  (`"no > prompt from modem"`, `"text mode rejected (+CMS ERROR: 500)"`, `"simulated"`, …)
  carried across the boundary verbatim, and only where the modem never got a chance to
  have an opinion is it ours (`"core service is gone"`, `"no answer from the core"`).

**Modem** (engineering). `ui->modem == NULL` becomes `!nd_svc_modem_present(ui)`, and
`nd_modem_status_snapshot()` becomes `nd_svc_modem_status()`. RADIO and DATA are fully
live; SIM is as §4 describes.

**FuelGauge** (engineering). The guard becomes
`!nd_svc_battery_present(ui) || !nd_ui_status_battery_hardware(ui)` — the second half is
unchanged on purpose, because `nd_ui_sim.h`'s override is what makes `eng-fuelgauge.png`
exist at all. `nd_ui_status_battery_hardware()` and `nd_ui_status_signal_level()` gain one
more fallback step, consulted **only** when the override is inactive *and* the direct
handle is NULL. The three per-refresh reads become one `nd_svc_battery_read()`; the
quick-start key becomes `nd_svc_battery_quickstart()`.

---

## 8. What is not done here

- **PB-5 / DL-10 stay open.** PhoneBook's "Call" still draws its own three-line
  "Calling…" screen and never dials. Dial is deliberately *not* on this wire (§4), and
  the rest of that question — whether the hand-drawn screen should be replaced by the real
  `call_screen`, which changes pixels — is still the owner's.
- **The Modem app's SIM page** is degraded, not served (§4).
- **The "alive and not reading" write deadline is reasoned, not tested.** Filling a
  `SOCK_SEQPACKET` buffer needs a client that posts without reading, and the client half
  is synchronous by construction — so no test in the suite can build that state without
  hand-crafting wire records the header deliberately does not export.
- **No hardware has run this.** There is no modem and no fuel gauge on the build host.
  Everything below the service boundary — the CMGS `>` prompt, the `+CMGS` ack, the i2c
  register reads — is the same code as before and is exercised only by the existing corpus
  and pty tests. What the new tests prove is the boundary: that a real child process, run
  by a real `nd_proc_launch_app()`, reaches the core's real `nd_modem` and `nd_battery`
  objects and gets the right answers back.

---

## 9. The fifth operation: taking the phone down

*Design for `reboot` and `poweroff` on the service socket. Pays off three of the five
names `nd_proc.h` writes down as a debt in `nd_proc_app_needs_root()`: `Power`, `Update`
and `Downgrade` all leave `ROOT_STOCK_APPS` and run as `ndusr` like everything else.*

Section 4 above says four operations and no more, and that any fifth must have the same
argument made again. This section is that argument, followed by the plan. **Read §9 before
citing §4** — §4 is left as it was written, because it is the record of the argument at
the time and rewriting it would erase the thing a later reader needs.

### 9.1 The state this starts from

`nd_proc.c` drops every app to `ndusr` except five stock paths, each of which keeps root
for exactly one operation it cannot route anywhere:

```c
ND_PATH_APPS_DIR "/Power",     /* poweroff, reboot */
ND_PATH_APPS_DIR "/Update",    /* reboot, to finish installing */
ND_PATH_APPS_DIR "/Downgrade", /* reboot */
ND_PATH_APPS_DIR "/Clock",     /* settimeofday */
ND_PATH_APPS_DIR "/Settings",  /* neodct-sdcard format */
```

Three of the five want the same two verbs. `Downgrade` wants them only transitively: it
has no halt code of its own at all, it `dlopen()`s `apps/Update/app.so` and calls
`nd_update_install()`, which ends in `nd_update_reboot()`. So `Downgrade` costs nothing
extra and falls out the moment `Update` does.

Everything else those three apps do already works unprivileged, and that was checked
rather than assumed:

| what it needs | why `ndusr` can already do it |
| --- | --- |
| `/NeoDCT/User/.ndsys` (`pending.prop`, `boot_recovery`) | `S00userdata` creates it `0700 ndusr:ndusr` |
| `/NeoDCT/User/db` (the backup's source) | `0750 ndusr:ndusr` |
| the card (`backup_db`, the `.ndsw`) | `neodct-sdcard` mounts p1 `uid=ndusr` |
| `sync` | `sync(2)` needs no privilege |
| the network (`Downgrade`'s release list) | ordinary sockets |
| `dlopen` of another app's `app.so` | world-readable on the squashfs |

So the premise of the task holds: for these three, reboot and poweroff really are the
whole of it.

**Deleting the three names without adding the verb does not fail loudly, which is why it
has not simply been done.** `nd_power_which()` resolves `poweroff` by walking `$PATH` with
`access(X_OK)`, and `/sbin/poweroff` is `0755` on the squashfs — so an `ndusr` Power app
*finds* the binary, `nd_power_spawn_first()` *succeeds* (the fork worked), and the failure
happens in the child, after the exec, when busybox tries to signal PID 1 and gets `EPERM`.
The app then sits through its thirty-second dwell and returns to the launcher. No dialog,
no log line, no clue. A phone that cannot be switched off and does not say so is worse
than one that keeps root.

Worth knowing while reading the rest: `tools/nd_selftest.c` does **not** consult
`nd_proc_app_needs_root()`. It reports `FAIL a stock app is running as root -- it did not
drop` for anything under `/NeoDCT/System/apps/` that is not `ndusr`, which means
`nd-selftest processes` already reports these three as failures today. It needs no change;
this work is what makes it stop being right.

### 9.2 Why "reboot the phone" is safe to expose when "hang up the call" was not

Four arguments, in the order of how much weight they carry.

**1. An app can already deny the phone to its owner; it cannot already end a call.**

This is the one that decides it. The capability actually being handed over is *make the
phone unavailable for about forty seconds, losing nothing that survives a battery pull*.
An app process already holds the framebuffer descriptor and the key channel, and the core
only signals it on `RING` — so a hostile app can already paint whatever it likes forever
and ignore the user. It can already exhaust 64 MB. The reboot verb does not add denial of
availability to an app's reach; it adds a *tidy, self-announcing, logged* form of
something already reachable by half a dozen untidy ones.

`nd_modem_hangup()` is not like that at all. An app that crashes does **not** drop a call:
the modem holds it, the core survives, the crash screen appears over it and the call
continues. Hang-up reaches a live external resource whose state cannot be reconstructed
from this side, and it is silent — the other party hears the line go dead and the owner
cannot tell it from a network drop. It is a genuine escalation. Reboot is not.

The same test rules out the rest of `nd_modem.h` unchanged: `dial` spends the owner's
money (`SECURITY-AUDIT.md` §4 Q1), `answer` opens a microphone, `send_at` is the whole
modem in one string. None of them is reachable any other way, and all of them stay off.

**2. The reach of the socket is one process, chosen by the core, for one launch.**

`NEODCT_SERVICE_FD` is one end of a `socketpair()` with no name in the filesystem, created
inside `nd_proc_launch_app()` and destroyed with the launch. It is not a listening socket,
it cannot be connected to, and it cannot be inherited by anything the app did not fork
itself. "An app can reboot the phone" therefore means "the app the user just opened from
the menu can reboot the phone", and `nd_proc.h` guarantees there is only ever one of them.
It does not mean "anything running on the phone".

**3. The surface after this change is strictly smaller than the surface before it.**

Today `Power`, `Update` and `Downgrade` are root with the whole filesystem: they can write
`/NeoDCT/User/.remote/state.prop` and get a reverse shell on every boot
(`SECURITY-AUDIT.md` §4 Q5 vector 1), open `/dev/ttyUSB2` and dial, read the ssh keys, and
`execve` anything on the image. This change takes all of that away and leaves them one
verb. The three apps that gain the verb are exactly the three that had *everything*.

What is genuinely new is that the other twenty-odd stock apps can now ask too. That is the
only thing worth arguing about, and argument 1 is the answer to it.

**4. The core keeps the decision, and there is an audit trail.**

A hang-up verb would make the core a pass-through: the app names the action and the core
performs it verbatim. Power is not like that. The core owns the candidate walk, the
ordering, the `sync`, the refusal when nothing can be spawned, and the one-shot recovery
flag — and it logs the requesting app's path on `ND_LOG_POWER` before it acts, so a phone
that restarts on its own has an answer on the serial console. That is the difference
between an abstraction and a hole with a function name on it.

**What this is not.** It is not enforcement. `SECURITY-PLAN.md` §3 is explicit that a
library check before the direct path is removed is "a permission system that politely
asks", and this is one: an `ndusr` app that wanted to could still try to spawn `poweroff`
itself. It will fail, because `ndusr` has no `CAP_SYS_BOOT` — and *that* is the boundary.
The verb is the supported route through a boundary the kernel already enforces, not the
boundary itself. Nothing here should be described as making the phone safer; it makes the
three apps that were root stop being root, which is a different and smaller claim.

### 9.3 One operation, three actions

`Power` offers Power off, Reboot and Recovery. All three are modelled.

```c
typedef enum {
    ND_SVC_POWER_OFF      = 0,
    ND_SVC_POWER_REBOOT   = 1,
    ND_SVC_POWER_RECOVERY = 2
} nd_svc_power_action;
```

One op with an action field, not three ops: it is one entry in the server's switch, one
validator, one latch, and it says the true thing — these are three shapes of a single
capability, and an app that has one has all three. Three ops would suggest they could be
granted separately, which nothing in this design can do.

**Recovery is modelled here rather than left half in the app, and the reason is not
privilege.** `/NeoDCT/User/.ndsys` is `0700 ndusr:ndusr`, so a dropped `Power` app could
still write `boot_recovery` itself; moving the write to the core buys no confinement at
all and this document should not pretend otherwise. It is modelled for two other reasons:

- **It fixes an ordering bug that ships today.** `request_recovery()` writes the flag and
  *then* tries to reboot. If the reboot cannot be started the flag stays on the partition,
  and the next ordinary restart — hours later, for an unrelated reason — silently boots
  into recovery. Doing it in the core lets the order be right: resolve the halt binary
  first, write the flag second, so a flag is never left behind by a reboot that could not
  begin.
- **The flag is a contract with the initramfs, not an app's file.** `ndsys-recovery.sh`
  reads it before any partition is mounted and deletes it as it reads. That belongs beside
  the halt, in one place, not in one app's `main.c`.

### 9.4 Reply before the act — which is what the timeouts now mean

A reboot request that is answered *after* the deed never gets answered: the machine goes
away mid-flight, the app's `recv()` returns `EOF` or times out, `svc_call()` maps that to
`SVC_ST_UNAVAILABLE`, and the last thing on the screen as the phone shuts down says
**"Power off failed."** That is the wrong sentence, produced by the correct code, and it
is the failure mode this whole section exists to design around.

So the ordering is fixed and it is load-bearing:

1. validate the record (`valid_request()`, as for every other op);
2. do **everything that can fail and is still undoable** — resolve the halt binary for
   this action, and for `RECOVERY` create the one-shot flag, *in that order*;
3. **send the reply**;
4. only then `sync()` and spawn.

Step 2 before step 3 is what makes "Power off failed." a real answer rather than a guess:
by the time the reply leaves, the only remaining failure is `fork()` itself. Step 3 before
step 4 is what makes "no answer from the core" never mean "it worked".

Consequences, which answer the question the task asks directly:

- **No new timeout constant.** The core's work before it replies is a `mkdir`+`open` and
  at most nine `access()` calls: microseconds. `ND_SVC_TIMEOUT_S` (5 s) is a real deadline
  on a real answer here, not a race with a shutdown, and `ND_SVC_SMS_TIMEOUT_S`'s reasoning
  ("it must sit above the core's own worst case") does not apply because there is no worst
  case to sit above.
- **A timeout is a genuine failure and the app should say so.** With the reply ordered
  before the act, "I did not hear back" can no longer be caused by success. It means the
  core is wedged, and `Power` drawing its existing failure dialog is correct.
- **`true` means accepted, not done**, and the header says so in those words. After a
  `true` return the app has nothing left to do and no way to find out any more.
- **The window between accepting and spawning can still fail** — `fork()` returning
  `ENOMEM` — and then the app has been told "accepted" and the phone stays up. It dwells
  thirty seconds and returns to the menu, which is exactly what the Python does today when
  `subprocess.Popen(["reboot"])` succeeds and the child then fails. Not fixed, because
  fixing it means a second round trip after the deed, and the deed is the machine leaving.

### 9.5 There is no direct path, and that is the point

`nd_svc.h`'s headline rule is that a direct handle always wins: every existing call checks
`ui->modem` or `ui->battery` first and touches the socket only when there is none. **The
power call deliberately does not do this**, and the exception has to be written into the
header beside the rule or someone will "fix" it.

For the modem and the battery, the direct path is *the same operation on the same object*
— which is why `nd-shoot`, running apps in-process with live handles, captures frames
through byte-for-byte the code that produced the references. For power there is no object.
The direct path would mean "the process that asks is the process that halts", and the two
processes that hold direct handles are the core, which never asks, and **`nd-shoot`, which
runs real app code in-process on a developer's laptop.**

So the rule inverts: the power call goes over the wire or it fails. `power.h` currently
documents a hole —

> nd_power_go_down(). It runs `sync` and then the first of `poweroff`, `/sbin/poweroff`,
> `busybox poweroff` that exists -- on a developer's machine that is a real poweroff, and
> a test suite that can switch off the machine it is running on is not a test suite.

— and this change closes it rather than restating it. After it, `nd_power_go_down()` in an
unwired process does nothing but return `false`, and **the Power app becomes testable end
to end for the first time.**

### 9.6 Arming: how a build host is stopped from switching itself off

The client half is safe by construction (§9.5). The *server* half is not: `test_svc.c`
already forks a real child through the real `nd_proc_launch_app()`, and
`test_loopback_over_a_live_server()` drives a real server thread against a real `nd_ui` in
the test process. Either could reach a halt.

The guard is a sink the process must install, defaulting to none:

```c
/* nd_power.h */
typedef bool (*nd_power_sink)(nd_power_action action, const char *exe,
                              const char *const *argv);

void nd_power_arm(nd_power_sink sink);   /* NULL disarms */
bool nd_power_is_armed(void);

/* The shipped sink: sync(2), then nd_proc_spawn() with ND_OWNER_SYSTEM. */
bool nd_power_spawn_sink(nd_power_action action, const char *exe, const char *const *argv);
```

`core/nd_main.c` calls `nd_power_arm(nd_power_spawn_sink)` **once, in `main()`, before any
thread exists** — beside `nd_proc_reaper_start()`, which is the same kind of statement
about what this process is. Nothing else in the shipped image calls it, so `nd-apprun`,
`nd-shoot`, `nd-selftest` and every unit-test binary are disarmed and physically cannot
halt the machine they run on. It fails closed: a build that forgets the line has a phone
that will not switch off, which is loud, rather than a test suite that switches off a CI
runner, which is not.

A function pointer rather than a boolean because it is also the seam the tests need: a test
installs a recording sink, drives the whole wire, and asserts *what would have been spawned*
without spawning it. The one thing no test may run is then exactly the one thing a test may
replace, and the untested surface shrinks from a whole app to two lines inside
`nd_power_spawn_sink()`.

It is not thread-safe and does not need to be — set once, before threads, read afterwards.
The header says so.

The server reports the arm state as `present`, so an app on a disarmed core gets the same
three-way answer every other operation gives: *present and accepted* / *present and
refused, with a reason* / *no route at all*.

### 9.7 The interlock: there is no lock, and adding one would be worse

The requirement is that the core must not take the phone down while an update is mid-write.
Four facts, and then what is actually built.

1. **The app's own writes are ordered by its own program counter.** `Update` calls
   `nd_update_stage()`, which returns only after `nd_upd_stage_package()` and `run_sync()`
   have both finished; *then* it draws the Ready page; *then* the user presses Restart;
   *then* it asks. It is single-threaded and synchronous. No lock can improve on that, and
   this is unchanged by the verb — before it, the app spawned `reboot` at exactly the same
   point.
2. **The core writes nothing to the user partition while an app child runs.**
   `OPEN-QUESTIONS.md` X-13: the pump loop calls `nd_input_read_event()` directly and ticks
   no service, so `battery_tick`, `modem_tick` and `calendar_tick` — and with them every
   sqlite write and every `settings.prop` write — do not run for the whole life of the
   launch. The `ClockService` worker is the only other live thread and it writes no file
   (`clock_settime` and an RTC `ioctl`, neither of which is a filesystem).
   **This is now a precondition of the halt path, not just of the battery**, and it is
   recorded in `nd_svc.c` beside the existing note that says the battery needs a mutex the
   day the pump loop grows a tick. Add: and the halt needs a quiesce the same day.
3. **There is only one app child** (`nd_proc.h`), and the socket dies with the launch. So
   there is no second app that can pull the phone down under `Update`'s feet — the only
   process that can ask is the one that is writing, and it is not asking while it writes.
4. **The staging record is already crash-atomic.** `staging.c`'s header: the record is
   removed first and the stale image second, then the new record is written
   temp-file → `fsync` → `rename`, and the directory is `fsync`ed so the rename survives.
   A halt at any instant reads back as either "nothing pending" or "a complete pending
   record", never a torn one. The verb does not weaken this and does not need to defend it.

So what is left is not a lock, it is the page cache. Every writer on this phone has
returned from `write(2)` with its pages still dirty, and the only thing that has ever
addressed that is `sync`. **The core calls `sync(2)` itself, inside `nd_power_spawn_sink()`,
after the reply and before the spawn.** It must be the core's `sync` and not the app's:
the app's proves only that the app's own writes landed, and after this change the app
cannot ask for the core's sqlite pages, `settings.prop` or the notify state to be flushed.
`sync(2)` is unprivileged, flushes every mounted filesystem, and cannot fail — so it is a
direct call, not a spawned `/bin/sync`.

The apps keep their own `run_sync()` calls. They still work as `ndusr`, they are the
Python's, and an earlier flush on ubifs is a small real benefit; they are simply no longer
load-bearing.

**And the design that is deliberately refused:** a `BUSY`/`IDLE` pair of verbs, so an app
can tell the core "do not halt while I am writing". It would be a lock held by the least
trusted process in the system, and an app that takes it and then crashes, or never releases
it, leaves a phone that cannot be switched off. That trades a hazard that cannot happen
(1–3 above) for one that can, and hands the lever to the wrong end. If a future core ever
does write while an app runs, the answer is for the *core* to quiesce its own writers on
the way down — which it can do, because they are its threads — not to ask permission from a
child.

### 9.8 The confirmation stays in the app, and the core must not grow one

Three reasons, and the first is structural rather than a preference.

**The core cannot draw.** While an app runs, the core's UI thread is inside
`nd_proc_launch_app()`'s pump loop forwarding keys to the child, and the child owns the
framebuffer. There is no "core dialog over a running app" anywhere in this design — the
core would have to stop forwarding keys, take the panel back, draw, read the keypad
itself, and then hand all three back. That is a much larger change than this verb and it
would have to be got exactly right on a matrix keypad with no kernel queue.

**The question is different in each of the three apps, on purpose.** `Power` asks "Switch
the phone off?" / "Restart the phone?" / "Restart into recovery?" in a `MessageDialog`.
`Update`'s confirmation *is* the Restart softkey on the Ready page, with `cancel_keys=()`
because there is deliberately no way back from there. `Downgrade` never asks at all,
because `Update`'s page already did. A core-side dialog would either duplicate all three or
replace them, and replacing them changes three shipped screens to buy nothing.

**It would not be a security control.** A confirmation defends against a hostile app only
if the user reads it — and a hostile app owns the panel and can draw a convincing one of
its own. Against an *accidental* reboot, the app-side confirmation already exists and is
already right.

So: the core's contract is written in `nd_svc.h` as *"this is not a confirmation prompt;
ask before you call it"*, and what the core does owe instead is the log line naming the
app that asked.

### 9.9 Does the app still need to exist afterwards

Yes, and it keeps its dwell.

`nd_app.h`'s teardown contract is unaffected: after a `true` return the app returns to its
own loop and sits in `nd_power_dwell()`, which is `nanosleep()` in 0.1 s slices polling
`nd_app_should_exit()`. The dwell exists for the reason `Power`'s comment gives — *"init
takes a moment to bring everything down. Sit here instead of returning to the launcher,
which would look like the key did nothing"* — and that reason is unchanged by moving the
spawn. `nd_vclock_enabled()` still skips it, so capture mode does not sit through it.

What happens next is the same as today: `poweroff` signals PID 1, busybox init `SIGTERM`s
everything, `nd-apprun`'s handler sets the flag, `app_shutdown()` releases the sound card,
`::shutdown:/bin/umount -a -r` runs. The app being alive for those few seconds is what
makes that orderly, and it is why the core spawns `poweroff` rather than calling
`reboot(2)` directly — `reboot(2)` from the core would skip init's shutdown entirely and
take an ext4/ubifs user partition down without unmounting it. **The candidate list moves
into the core unchanged, in the Python's order, and nothing calls the syscall.**

The core does *not* `SIGTERM` the app child when it accepts a halt. It could — it has
`nd_proc_terminate()` — but killing the app that is drawing "the phone will restart" so
that the core can draw something else instead is a redesign of what the user sees, not a
port of it.

### 9.10 The wire

```c
typedef enum {
    SVC_OP_SEND_SMS = 1,
    SVC_OP_MODEM_STATUS = 2,
    SVC_OP_BATTERY = 3,
    SVC_OP_BATTERY_QUICKSTART = 4,
    SVC_OP_POWER = 5
} svc_op;
```

**Request.** One field is added to `svc_req`, after `pad`:

```c
uint32_t action;   /* SVC_OP_POWER only; nd_svc_power_action */
```

Appended rather than folded into the existing `pad`, so that `sizeof(svc_req)` changes and
the `size` field catches a half-updated build. (It cannot happen — an `.ndsw` replaces the
whole rootfs at once, so `app.so` and `libneodct.so` always move together — but that check
is cheap and this is exactly what it is for.) `SVC_VERSION` goes to `2`: it is the one
field whose entire job is to say that the meaning of these bytes changed, and this changes
the meaning of these bytes.

**Validation**, in `valid_request()`, alongside the existing cases:

```c
case SVC_OP_POWER:
    return r->action <= (uint32_t)ND_SVC_POWER_RECOVERY;
```

That is the whole of it, and it is worth noting how little there is to check compared with
`SEND_SMS`. There is no attacker-influenced string here: the request is one enum out of
three, and everything the core executes — the binary names, the argv, the flag path — is a
`static const` table compiled into `libneodct.so` on the read-only rootfs. The app cannot
name a program, cannot name a file, and cannot influence a single byte of what is spawned.
That is the shape §4 asked for when it refused the SIM page: *"a fifth op that runs a fixed
list of commands core-side and returns the parsed fields — never a passthrough where the
app chooses the string."*

**Response.** No new fields. `status`, `ok`, `present`, `err` and `detail` already carry
everything:

| field | meaning for `SVC_OP_POWER` |
| --- | --- |
| `status` | `SVC_ST_OK` once the record was well-formed; `SVC_ST_BAD_REQUEST` for an action out of range |
| `present` | the core is armed (§9.6) — `0` in `nd-shoot`, a test, or a hand-run core |
| `ok` | **accepted**: the binary resolved, the flag (if any) was written, and the sink will be called as soon as this reply is on the wire |
| `err` | the `errno` from a recovery flag that could not be written; `0` otherwise |
| `detail` | the reason, e.g. `"no poweroff on this image"` or `strerror()` of the flag failure |

### 9.11 The serving thread grows a deferred act

`serve()` cannot perform the halt, because `svc_thread()` sends the reply *after* `serve()`
returns and §9.4 requires the reply to go first. So `serve()` gains an out-parameter:

```c
typedef struct {
    bool pending;
    nd_power_action action;
    char exe[ND_PATH_MAX];        /* the resolved absolute path */
    const char *const *argv;      /* points into nd_power.c's static tables */
} svc_deferred;

static void serve(nd_ui *ui, const svc_req *req, svc_resp *out, svc_deferred *after);
```

and `svc_thread()` becomes:

```c
serve(s->ui, &req, &resp, &after);
if (!svc_send(s->fd, &resp, sizeof resp, svc_now() + ND_SVC_TIMEOUT_S))
    break;
if (after.pending) {
    /* THE REPLY IS ALREADY ON THE WIRE. Everything below is irreversible
     * and nothing below it can be reported to anybody. */
    if (nd_power_commit(&after))
        break;   /* the phone is going down; there is nothing left to serve */
}
```

Two notes on that:

- The `break` on success stops a second request being served by a core that is on its way
  out, and lets `nd_svc_server_stop()` take the fast path.
- `nd_svc.c` keeps a process-global latch, set **only after a sink returns true**, so a
  repeat request during a slow shutdown is answered `ok` without spawning a second
  `poweroff`. Set only on success, because a latch set by a halt that failed to start would
  leave a phone that can never be rebooted — which is the same failure mode §9.7 refuses a
  BUSY verb for.
- `ND_PATH_MAX` is 512, so `svc_deferred` adds about half a kilobyte to the serving
  thread's frame. `nd_svc_server_start()`'s comment currently says one iteration holds
  "about 2.6 KB" against a 128 KB stack; it becomes about 3.2 KB and the comment is updated
  rather than left to rot.

### 9.12 The API

> **What was actually built differs from this subsection, and the code is the
> authority.** This design proposes a separate `nd_power.h` module with an
> `nd_power_action` enum and an arming sink. The implementation put the same
> mechanism in `nd_svc.h` instead — `nd_svc_reboot()`, `nd_svc_poweroff()`,
> `nd_svc_halt_which()`, `nd_svc_halt_simulate()` — because the halt has no
> `nd_ui *` handle to hang a direct path on, so it never needed a module of its
> own; the candidate tables and the `execvp` walk still moved out of the two
> `app.so` copies into `libneodct.so`, which was the point.
>
> Everything ARGUED for in 9.1 through 9.11 landed unchanged: reply before the
> act, `sync(2)` between the reply and the spawn, no interlock, the
> confirmation staying in the app, and the recovery flag still written by the
> app process because `.ndsys` is `0700 ndusr:ndusr` and a dropped app can
> write it. The subsection below is kept as the design that was reasoned to,
> not corrected into a description of the result — 9.13's file list is
> likewise the plan rather than the diff.


**`neodct/src/include/nd_power.h`** — new. The halt, as a library module, so that the two
copies of the candidate walk that live in `apps/Power/main.c` and `apps/Update/main.c`
today (`Update`'s header apologises for the duplication: *"It is a copy of
nd_power_which(); the two apps are separate shared objects and cannot share it"*) become
one copy in `libneodct.so`, which both already link.

```c
typedef enum {
    ND_POWER_ACT_OFF      = 0,
    ND_POWER_ACT_REBOOT   = 1,
    ND_POWER_ACT_RECOVERY = 2
} nd_power_action;

#define ND_POWER_STATE_DIR     "/NeoDCT/User/.ndsys"
#define ND_POWER_RECOVERY_FLAG "/NeoDCT/User/.ndsys/boot_recovery"
#define ND_POWER_WHY_MAX       160
#define ND_POWER_CANDIDATES    3

extern const char *const *const nd_power_halt_commands[ND_POWER_CANDIDATES];
extern const char *const *const nd_power_reboot_commands[ND_POWER_CANDIDATES];

/* execvp's lookup half, moved verbatim from apps/Power/main.c. */
bool nd_power_which(const char *name, char *out, size_t out_sz);

/* Everything that can fail and is still undoable, in the order that matters:
 * resolve the binary for `action` FIRST, then (RECOVERY only) create the
 * one-shot flag -- so a flag is never left behind by a halt that could not
 * be started. `why` receives strerror()/reason text; pass NULL to discard.
 * On ND_OK, `exe` holds the resolved absolute path and *out_argv points at a
 * static table owned by this module and never freed. */
nd_err nd_power_prepare(nd_power_action action, char *exe, size_t exe_sz,
                        const char *const **out_argv, char *why, size_t why_sz);

/* The one-shot recovery flag on its own, for a caller that wants it without
 * a halt. mkdir -p + create empty + truncate, as the Python's
 * `with open(RECOVERY_FLAG, "w"): pass`. */
nd_err nd_power_write_recovery_flag(char *why, size_t why_sz);

/* The irreversible half, replaceable so the one thing no test may run is the
 * one thing a test may substitute. See spec-app-services.md 9.6. */
typedef bool (*nd_power_sink)(nd_power_action action, const char *exe,
                              const char *const *argv);
void nd_power_arm(nd_power_sink sink);
bool nd_power_is_armed(void);
bool nd_power_spawn_sink(nd_power_action action, const char *exe, const char *const *argv);

/* time.sleep(seconds) in slices, polling nd_app_should_exit(), skipped under
 * the virtual clock. Three identical static copies in apps/Power,
 * apps/Update and (via dwell) elsewhere collapse into this one. */
void nd_power_dwell(double seconds);
```

**`neodct/src/include/nd_svc.h`** — the fifth operation.

```c
typedef enum {
    ND_SVC_POWER_OFF      = 0,
    ND_SVC_POWER_REBOOT   = 1,
    ND_SVC_POWER_RECOVERY = 2
} nd_svc_power_action;

/* One round trip's worth of answer, in the shape nd_svc_battery already
 * argues for: the caller renders three different sentences and a bool plus a
 * string cannot carry which. */
typedef struct {
    bool accepted;    /* the core has taken responsibility; NOT "it happened" */
    bool present;     /* this core can halt anything at all (it is armed)     */
    bool flag_failed; /* RECOVERY only: the one-shot flag could not be left   */
    char detail[ND_MODEM_DETAIL_MAX];
} nd_svc_power;

/* As nd_svc_modem_present(), for the halt. NOTE: unlike every other call in
 * this header this one does NOT consult a direct handle -- there is none,
 * and the processes that hold direct handles are the core (which never asks)
 * and nd-shoot (which runs on a developer's laptop). See section 9.5. */
bool nd_svc_power_present(const nd_ui *ui);

/* Ask the core to take the phone down.
 *
 * TRUE MEANS ACCEPTED, NOT DONE, and it is the last thing this app will ever
 * learn: the core replies before it acts precisely so that "no answer" can
 * never be the normal case. THIS IS NOT A CONFIRMATION PROMPT -- the core
 * cannot draw while an app owns the panel, so asking the owner is the
 * caller's job and it has already happened by the time this is called.
 * *out is always written, zeroed first, so a caller that ignores the return
 * still renders something. */
bool nd_svc_power_request(const nd_ui *ui, nd_svc_power_action action, nd_svc_power *out);
```

### 9.13 What each file becomes

| file | change |
| --- | --- |
| `src/include/nd_power.h` | **new** — §9.12 |
| `src/lib/nd_power.c` | **new** — the tables, `nd_power_which()`, `nd_power_prepare()`, the flag, the sink and the arm, `nd_power_dwell()`. Bodies moved from `apps/Power/main.c`, not rewritten |
| `src/lib/nd_svc.c` | op 5, the `action` field, `SVC_VERSION` 2, the validator case, the `serve()` out-param, the deferred act in `svc_thread()`, the latch, `nd_svc_power_present/_request`, the stack-size comment, the X-13 precondition note |
| `src/include/nd_svc.h` | the fifth operation and the argument for it; the "FOUR OPERATIONS" block becomes five with §9.2 in short form; the "a direct handle always wins" block gains its one exception |
| `src/core/nd_main.c` | one line: `nd_power_arm(nd_power_spawn_sink);` in `main()`, beside `nd_proc_reaper_start()` |
| `src/lib/nd_proc.c` | `ROOT_STOCK_APPS` loses `/Power`, `/Update`, `/Downgrade`; the comment above it says what the remaining two are still waiting for |
| `src/include/nd_proc.h` | "ONE OF FOUR STOCK APPS" (the text says four and lists five — fix that too) becomes two: `Clock` (`settimeofday`) and `Settings` (`neodct-sdcard format`) |
| `src/include/nd_app.h` | two sentences that enumerate "the four things" and "four operations" |
| `src/apps/Power/main.c` | `nd_power_which`, `spawn_inherit`, `nd_power_spawn_first`, the six candidate tables, `run_sync`, `dwell` and `nd_power_request_recovery_flag` all deleted. `nd_power_go_down()` becomes `(nd_ui *, nd_svc_power_action, const char *failure)` and is three statements: request, dialog on refusal, dwell. `request_recovery()` collapses into one `go_down(ui, ND_SVC_POWER_RECOVERY, ...)` whose failure branch picks "Cannot ask for recovery: %s" when `flag_failed` and "Reboot failed." otherwise |
| `src/apps/Power/power.h` | loses the tables and the moved prototypes; the "WHAT THE TEST DELIBERATELY DOES NOT CALL" block is **deleted**, because the hole is gone (§9.5) |
| `src/apps/Update/main.c` | `nd_update_which`, its `REBOOT_*` tables and its `dwell` deleted; `spawn_inherit`/`run_sync` stay (the backup and the stage still use them); `nd_update_reboot()` becomes the service call plus `nd_power_dwell()`, still with no failure dialog, still the Python's |
| `src/apps/Update/update_app.h` | the reboot table declarations go; note 3 in the file header ("It is a copy of nd_power_which()") is deleted because it is no longer true |
| `src/apps/Downgrade/*` | **nothing**. It reaches the halt only through `nd_update_install()` |
| `overlay/NeoDCT/CHANGELOG.txt` | one paragraph under `Unreleased` — the owner-visible fact is that the three apps that could read everything on the phone no longer can |
| `docs/c-rewrite/SECURITY-PLAN.md` §8 | "A shrinking list of stock apps" — record that it shrank, and to what |
| `docs/c-rewrite/OPEN-QUESTIONS.md` X-13 | the caveat gains the halt path beside the battery |

### 9.14 Tests

The point of the arm/sink split (§9.6) is that almost all of this is now testable on the
host. What is written:

**`test/unit/test_power_svc.c`** — new, `platform_test.h`, scratch `ND_ROOT`.

1. *disarmed is the default*: `nd_power_is_armed()` is false in a fresh process; a
   `nd_power_commit()` equivalent refuses and returns false. This is the case that keeps a
   CI runner alive and it runs first.
2. *`nd_power_prepare()` resolves*: with a scratch `$PATH` containing a fake `reboot` and
   no `poweroff`, `REBOOT` resolves to the fake and `OFF` returns `ND_ERR_NOTFOUND` with
   `why` set. Pins the candidate order — `poweroff` before `/sbin/poweroff` before
   `busybox poweroff` — which `power.h` calls load-bearing.
3. *ordering*: `RECOVERY` with **no** reboot binary anywhere leaves **no** flag on disk.
   This is the ships-today bug §9.3 names, and it is the reason recovery is modelled.
4. *`RECOVERY` with a binary* writes `/NeoDCT/User/.ndsys/boot_recovery` exactly once,
   empty, truncating; and on a read-only `.ndsys` returns the errno in `why`.
5. *the recording sink*: arm with a test sink, call the commit, assert `argv[0]` and the
   resolved path, assert the real sink was never entered, disarm.

**`test/unit/test_svc.c`** — extended, in the existing loopback fixture.

6. *an app on a disarmed core*: `nd_svc_power_present()` is false and
   `nd_svc_power_request()` returns false with `present == 0`. This is the shipped state of
   every test binary, so every other case in the file is protected by it.
7. *accepted, over the wire, with a recording sink armed*: `present`, `accepted`, and the
   sink saw `ND_POWER_ACT_REBOOT`. Proves the deferred act runs **after** `svc_send()` — the
   client returns before the sink fires, which the test observes by checking the sink's
   record only after the client call has returned.
8. *out-of-range action*: `action = 7` is `SVC_ST_BAD_REQUEST`, nothing is spawned, and the
   next request on the same channel still works (the existing "one refused request does not
   close the channel" assertion, extended).
9. *the latch*: two accepted requests, one spawn.
10. *no direct path*: with `ui->modem` and `ui->battery` non-NULL **and** no client channel,
    `nd_svc_power_request()` returns false. This is the `nd-shoot` case and it is the one
    that must never regress; it belongs next to `test_a_direct_handle_wins()` as its
    deliberate opposite.

**`test/apps/PowerApp/main.c`** — new test app.so, following `SvcApp`. A real child, forked
by the real `nd_proc_launch_app()`, asks for a reboot and writes what it got to a report
file the parent reads — the same shape as `test_a_real_child_reaches_the_core()`. The
parent runs disarmed, so the assertion is "the child reached the core and was told *no*,
with `present == 0`", which is a real round trip proving the plumbing without any risk to
the machine.

**`test/unit/test_proc.c`** — `nd_proc_app_needs_root()` for `/NeoDCT/System/apps/Power`,
`/Update` and `/Downgrade` flips from true to false; `Clock` and `Settings` stay true. The
existing table-driven case at line ~1057 is the one to edit.

**`test/unit/test_power.c`** — the tables and `which` cases move to `test_power_svc.c`; the
app-side cases that remain lose `test_spawn_first_with_nothing_to_spawn` and gain a case
that `nd_power_go_down()` on an unwired app draws the failure dialog rather than doing
anything. That case could not be written before this change.

**`test/unit/test_update_app.c`** — drops the `nd_update_reboot_commands` assertions.

**Not tested, and named rather than left to be discovered:** `nd_power_spawn_sink()` itself
— two statements, `sync()` and `nd_proc_spawn()`. That is the whole of the untested
surface, against a whole app today.

**On the phone.** `nd-selftest processes` is the acceptance test and it already asserts the
right thing (§9.1): open Power, then Update, and the FAILs it reports today become PASSes.
Then actually switch the phone off, which no host test can do.

### 9.15 What this does not do

- **`Clock` and `Settings` keep root.** `settimeofday` and `neodct-sdcard format` are two
  more verbs and two more arguments, and neither is this one. The list is shorter, not
  empty.
- **`nd_modem.h` is unchanged.** No dial, no answer, no hang-up, no raw AT. §9.2 argues why
  reboot is a different question, not why the others were wrong.
- **This is not enforcement.** See the closing paragraph of §9.2. The kernel refusing
  `CAP_SYS_BOOT` to `ndusr` is the boundary; this is the supported route through it.
- **The engineering-mode gate is still `settings.prop`.** Unchanged and still a hole, for
  the reasons `nd_proc.h` and `SECURITY-PLAN.md` §8 both already state at length.
- **No hardware has run this.** A build host has no `CAP_SYS_BOOT` story worth testing and
  no init to signal. What the host tests prove is the boundary and the ordering; that
  `poweroff` still switches a Luckfox off has to be seen on a Luckfox.

---

## 10. The last two: the clock, and formatting a card

*Design for `set_clock` and `format_card` on the service socket. Pays off the last two
names in `ROOT_STOCK_APPS`: `Clock` and `Settings` leave the list, **the list is empty**,
and no stock app on this phone runs as root.*

§4 says four operations and no more, and that any further one must have the same argument
made again. §9 made it for `reboot` and `poweroff`. This section makes it for the last
two, which start from a harder place than those did: **`reboot` and `poweroff` obviously
carried no arguments, and these two obviously did.** "Set the clock" names a time and
"format the card" named a device.

Most of what follows is about narrowing that. One of the two ends up carrying nothing at
all — `format_card` takes no device, because the core reads the card itself — and the
other carries a single integer inside a bound the core checks. So the finished API is
three argument-free verbs and one bounded number, which is not where it started.

**Read §10 before citing §4 or §9.15.** §9.15 says "`Clock` and `Settings` keep root. The
list is shorter, not empty." That was true when it was written and is the thing this
section changes. As with §4, it is left standing rather than corrected: it is the record
of where the work had got to, and a later reader needs it.

### 10.1 The state this starts from

After §9, `ROOT_STOCK_APPS` holds two names:

```c
ND_PATH_APPS_DIR "/Clock",    /* settimeofday */
ND_PATH_APPS_DIR "/Settings", /* neodct-sdcard format */
```

Each is one call. `apps/Clock/main.c` ends `show_clock_settings()` with

```c
nd_clock_set(when, "set by hand in the Clock app")
```

and `apps/Settings/main.c` ends `offer_format()` with `sdcard_format(card->device)`, which
is a `nd_proc_spawn()` of `/NeoDCT/System/hw/neodct-sdcard format <device>` followed by an
unbounded `nd_proc_wait()`.

Everything else both apps do already works unprivileged, and — as in §9.1 — that was
checked rather than assumed. `Settings` writes `settings.prop`, reads and writes
`/NeoDCT/User/wallpapers`, drives Bluetooth through `nd_bt`, and lists the card; all of
that is `ndusr`-owned or `ndusr`-readable already. `Clock` reads and writes
`system.clock.ntp_sync` in `settings.prop` and formats strings.

Unlike §9's case, **deleting these two names without adding the verbs fails loudly**, in
both directions and honestly:

| app | what an `ndusr` build does | what the user sees |
| --- | --- | --- |
| `Clock` | `clock_settime()` returns `EPERM` (no `CAP_SYS_TIME`) | "The clock would not take it." |
| `Settings` | the helper execs and fails: `umount`/`mount` need `CAP_SYS_ADMIN`, and `/run/neodct` is not `ndusr`-writable | "Formatting failed." |

Note which call fails in the second row, because it is not the obvious one. **`ndusr` is
in the `disk` group** (`configs/users-table.txt`), so `mkfs.vfat` on the card device
itself would very likely *succeed*; what an unprivileged helper cannot do is unmount the
card first, mount the result, and publish the new state to `/run/neodct/sdcard.prop`. An
`ndusr` `Settings` would therefore not fail cleanly — it would fail *after* writing a
filesystem. That is an argument for the verb rather than against it, and it is the reason
the row says what it says.

So the "silent nothing" problem that made §9.1 refuse to just delete the names does not
arise here: both failures reach the user. The reason to add verbs instead of accepting
the loss is that a phone whose clock cannot be set and whose card cannot be formatted is
a worse phone — and, for the card, that the halfway failure above is worse than either.

### 10.2 Setting the clock: what an app can reach by choosing the time

The clock **already moves without anybody asking**. `nd_clock_start()` applies a floor at
boot from `version.prop`'s build epoch, then syncs over SNTP on a detached thread. So "a
process decided what time it is" is the normal case and not a new power. What is new is
an *app* choosing the value, and possibly choosing a hostile one.

Two things were checked rather than assumed.

**The release signature does not read the clock.** `nd_signing.c` has no `notBefore`, no
`notAfter` and no `time()` call, and neither does the initramfs gate in `ndsys-apply.sh`.
This is the one that would have settled the question the other way: if a certificate
validity window gated installs, then a clock an app could move would be a route to
installing something whose signing key had been revoked. It does not, so it is not.

**TLS does read it.** `nd_clock.h`'s first paragraph is about exactly this: a phone that
boots at the Unix epoch fails every "not valid before" check on the internet at once. The
dangerous direction is therefore **forward**: far enough ahead and an *expired*
certificate reads as current, so a download could come from a server whose key was
revoked. It still could not be installed — the `.ndsw` is signature-checked in the
initramfs, after the download, by code the running system cannot rewrite — but the
download itself is worth not handing over.

Hence the bound, which is the whole of the API's narrowing:

```
refuse  when < build epoch                          nothing legitimate predates the image
refuse  when > build epoch + ND_SVC_CLOCK_MAX_SKEW_S   ten years; the direction that ages
                                                       certificates out
```

Ten years is far more than a person correcting a date needs and far less than the
multi-decade jump that expires a long-lived CA root. Between the two bounds the phone
believes its owner, which is the entire point of a clock app.

Three details of the bound that are load-bearing:

- **A missing `version.prop` does not mean no bound.** The floor falls back to
  `ND_CLOCK_SANE_MIN` (2020-01-01) — the value `ClockService` already trusts as "no real
  time is earlier than this" — rather than to zero.
- **A `version.prop` from before 2020 does not widen the window.** It is clamped up to
  `ND_CLOCK_SANE_MIN`. `version.prop` is machine-written but lives on the same partition
  as everything else, and a `1970` in it must not buy a clock in 1980.
- **The reason string is not on the wire.** The core logs
  `set by hand in the Clock app`, fixed at the call site. An app choosing that string
  would be writing whatever it liked into the core's log with the core's authority, and
  the `[CLOCK]` tag exists so that a person on a serial console can believe what moved the
  clock.

### 10.3 Formatting a card: the verb takes no device, and that is the design

`Settings` passed `card->device` to the helper. **A verb shaped that way would let any app
on the phone name a block device**, and the two most interesting ones here are the
partitions the phone is running from. `neodct-sdcard`'s own `is_reserved_device()` would
have refused those two — but "the helper checks" is a second line of defence, not a first,
and the point of moving the operation is to stop needing one.

So `nd_svc_format_card()` **takes no argument at all**. The core calls `nd_storage_card()`
itself and formats what it finds. There is no string to validate because there is no
string, which is the same shape §9 gave `reboot` and `poweroff` for the same reason.

Two refusals the core makes before the helper is reached:

| condition | why |
| --- | --- |
| `card.device` is empty | `ND_CARD_ABSENT` blanks it. Nothing to format. This is `Settings`' own "No card device to format." check, moved. |
| `!card.removable` | `removable` is `fstype != "virtiofs"`, and on QEMU the "card" is a directory on the developer's machine. `mkfs` on it is not something anybody meant to ask for. |

**Why allowing this at all is a smaller step than it looks.** An app can *already* destroy
the card's contents: `neodct-sdcard` mounts p1 `uid=ndusr`, so every file on it is one
`unlink()` away from any app on the phone. The verb adds "and rewrite the partition
table", which is the same loss by a faster route, not a new one. What it does **not** add
is reach: the format is confined to the one device the core found, and no app can move it.

**What is genuinely lost: the app can no longer cancel the format.** `Settings` held the
helper's pid and `SIGTERM`ed it from `app_shutdown()`, which ran when an incoming call
arrived mid-`mkfs`. The pid now lives in the core, so `app_shutdown()` is empty. On its
own terms this is an improvement — an `mkfs` killed half way leaves a card that mounts
nowhere — but it is a behaviour change and it is named here rather than left to be
discovered. If the core is stopped while a format is in flight, `nd_svc_server_stop()`
detaches the serving thread after `ND_SVC_JOIN_S` and the format runs to completion, which
is the same trade `ND_SVC_JOIN_S` already makes for an SMS on the wire.

### 10.4 What crosses the wire

Two new operation numbers, appended so that no number an existing build sends changes
meaning:

```c
SVC_OP_SET_CLOCK   = 7,
SVC_OP_FORMAT_CARD = 8
```

`SVC_OP_FORMAT_CARD` carries nothing. `SVC_OP_SET_CLOCK` adds one field to `svc_req`:

```c
int64_t when;   /* after the six uint32_t, so it lands 8-byte aligned with no hole */
```

`int64_t` and not `time_t`: the record goes over a socket and its layout must not depend
on how wide the compiler made `time_t`.

`valid_request()` gains three things:

1. `SVC_OP_SET_CLOCK` must carry a `when` inside `[ND_CLOCK_SANE_MIN, ND_CLOCK_SANE_MAX]`
   — the only part of the rule decidable from the record alone. The real bound is the
   build epoch, which is a file read and belongs with the operation.
2. `SVC_OP_FORMAT_CARD` is accepted with nothing further to check, as the two halt verbs
   are.
3. **Every operation that is not `SVC_OP_SET_CLOCK` must carry `when == 0`.** That is what
   every sender leaves it as, and enforcing it keeps "this op ignores that field" from
   quietly becoming "this op ignores that field *today*".

### 10.5 Timing

The clock verb uses `ND_SVC_TIMEOUT_S` (5 s): everything the core does is a bounded file
read and a syscall.

The format is the first operation on this channel that can take minutes. The old code did
not bound it at all — `nd_proc_wait(pid, -1.0, ...)` — so the two new constants are the
finite version of "forever":

```c
#define ND_SVC_FORMAT_WAIT_S    240.0   /* the core -> the helper */
#define ND_SVC_FORMAT_TIMEOUT_S 250.0   /* the app  -> the core   */
```

The ordering between them matters and is the same reasoning `ND_SVC_SMS_TIMEOUT_S` (45)
uses against the core's own 35-second worst case: **the side that knows why it failed must
give up first.** If the core times out it replies "failed" and `Settings` draws
"Formatting failed."; if the app timed out first it would close the channel and report a
transport failure for what is really a wedged `mkfs`. On a core timeout the helper is
terminated, because four minutes with no exit is a wedged helper and leaving it would keep
the serving thread in there for the life of the core.

Note what is *not* shared with §9.4's ordering rule. The halt verbs reply **before** they
act, because the act cannot fail in a reportable way and `sync(2)` is unbounded. These two
reply **after**, because the answer is the result: "did the clock take it" and "did the
format work" are the entire content of the reply.

### 10.6 Both sides run the same policy

`svc_halt()` follows the rule "a direct handle always wins" in its degenerate form: there
is no `ui->` handle for the phone, so a process with **no channel** — `nd-core` itself, a
hand-launched `nd-apprun`, `nd-shoot`, a unit test — does the work itself.

These two do the same, with one addition that matters: **the policy runs on whichever side
does the work.** `clock_in_bounds()` and the card refusals are inside
`clock_set_bounded()` and `format_card()`, which `serve()` calls for an app and the client
half calls for a process with no socket. A rule that only existed on the far side of a
socket would be a rule you could get out of by not having one.

### 10.7 Testing

**`test/unit/test_clock.c`** — the window, to the second. This is the file that can stage
a `version.prop` (`pt_new_case()` gives every case a fresh `ND_ROOT`), so it is the only
one that can pin the bound exactly:

1. inside the window is taken — the build epoch itself, a day later, and the last second
   of the window;
2. before the build epoch is refused, including `VCLOCK_NOW` (2024, against a 2026 build)
   and `0`;
3. one second past the ceiling is refused, and so is 2100;
4. with **no** `version.prop` the window still has a floor at `ND_CLOCK_SANE_MIN`;
5. a `version.prop` reading 2000-01-01 does not lower it.

Nothing here moves the machine's clock: `nd_clock.c` refuses `clock_settime()` while
`NEODCT_ROOT` is set, which it always is under this harness. Its `capture_begin()` was
extended to capture **stderr as well as stdout**, because a refusal is an error-level log
line and a case pinning it would otherwise have asserted nothing at all, silently.

**`test/unit/test_svc.c`** — the wire, in the existing loopback fixture:

6. a time inside the window crosses and is accepted;
7. `0` and `> 2100` are refused by the *client* half, before a record is built;
8. a day past the ceiling — inside 2020–2100, so it **crosses the wire** and is refused by
   the core. Without this case, 7 would still pass on a build whose server-side bound had
   been deleted entirely;
9. a format with a fake helper (`nd_svc_format_simulate()`) returning 0 succeeds, **and the
   device the helper was pointed at is the one in the state file the core read** — which is
   the assertion the whole case exists for, since the app cannot name one;
10. a helper returning 1 fails;
11. a `virtiofs` card is refused **and the helper is never called** — the call count is the
    proof, because `mkfs` on the developer's machine is not something a boolean could take
    back;
12. an absent card is refused, likewise without a call;
13. after every one of those refusals the channel is still good — a failed operation, not a
    protocol error.

`nd_svc_format_simulate()` is the sibling of `nd_svc_halt_simulate()` and exists for the
same reason `power.h` gives: a test suite that can repartition the machine it runs on is
not a test suite. It replaces the spawn-and-wait and **nothing else** — the validation, the
`nd_storage_card()` read, both refusals and the logging are all real. The clock verb needs
no partner to it, because `nd_clock_set()` has honoured `NEODCT_ROOT` since it was written.

**`test/unit/test_proc.c`** — `t_the_named_stock_apps_still_hold_root()` becomes
`t_no_stock_app_holds_root()`, and it is deliberately a loop over **every** stock app name
(`STUB_STOCK_APPS` from `src/Makefile`, which is what decides `apps/` versus
`engineering/apps/` and is therefore the real definition of "stock") rather than over a
remembered list: a name that reappears in `ROOT_STOCK_APPS` should fail whether or not
anybody thought to add it here. It asserts both `engineering_mode` values, because
engineering mode grants root by *location* and `/NeoDCT/System/apps` is not that location.
`t_the_stock_name_alone_grants_nothing()`'s last assertion flips from
`CHECK(nd_proc_app_needs_root(&real, false))` to `CHECK(!...)`; that one line is what these
two verbs bought.

**On the phone.** `nd-selftest processes` is the acceptance test and it needs no change:
it reports `FAIL a stock app is running as root -- it did not drop` for anything under
`/NeoDCT/System/apps/` that is not `ndusr`, so opening `Clock` and `Settings` and seeing
PASS is the whole of it. Then set the clock by hand, and format a card.

### 10.8 What this does not do

- **`ROOT_STOCK_APPS` is empty; it is not deleted.** The array and the loop stay, with the
  comment above it rewritten into a warning about what adding a name would mean. An empty
  policy costs one `NULL` pointer and no special case.
- **Engineering apps still run as root when engineering mode is on.** That is deliberate and
  is the *one* remaining reason any app on this phone is privileged. `RemoteShell` exists to
  hand a developer a root shell; confining it would not make the phone safer.
- **The engineering-mode gate is still `settings.prop`.** Unchanged, still a hole, and still
  written down as one in `nd_proc.h`. The second gate (`neodct.engmode=1` on the kernel
  command line, which the writable partition cannot set) is one `&&` away and is not this
  section's work.
- **`nd_modem.h` is unchanged.** No dial, no answer, no hang-up, no raw AT. §9.2 argued why
  taking the phone down is a different question; nothing here reopens that one.
- **This is not enforcement.** As §9.15 says: the kernel refusing `CAP_SYS_TIME` and
  `CAP_SYS_ADMIN` to `ndusr` is the boundary. These verbs are the supported route through
  it. SELinux is what turns "the core is allowed to" into "the core is allowed to *this
  much*", and that track is separate.
- **It does not close the `disk` group, which is now the loudest hole next door.**
  `configs/users-table.txt` puts `ndusr` in `disk`, and eudev's shipped
  `50-udev-default.rules` line 70 is `SUBSYSTEM=="block", GROUP="disk"` — with no `MODE`,
  which udev turns into `0660` whenever a group is set. So **every block device node on
  the phone is `root:disk 0660`, and every app now running as `ndusr` can open the system
  and user partitions read-write and rewrite them raw.** That was harmless while stock
  apps ran as root and it is not harmless now: it goes around the `/NeoDCT/User` mode-bit
  boundary entirely, and dm-verity turns a rewritten rootfs into a brick rather than into
  a compromise, which is better but not good.

  Nothing in `neodct/src` opens a block device from an app — the card is mounted by
  `neodct-sdcard`, which runs as root — so `disk` looks removable from `ndusr`'s group
  list. "Looks removable" is not "was tried on a phone", which is why it is written down
  here rather than done in this change. It wants: the group dropped from
  `users-table.txt`, an `nd-selftest` probe that opens each block node as `ndusr` and
  FAILs if it succeeds, and a boot.
- **No hardware has run this.** The host tests prove the bound, the refusals and the wire.
  That `mkfs.vfat` still partitions a real SD card through the core rather than through the
  app has to be seen on a Luckfox.

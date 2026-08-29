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

## 4. Scope of the API: five operations, and no more

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
| `ND_SVC_OP_MODEM_RESET` | — | ok? | `Modem/main.c` |
| `ND_SVC_OP_BATTERY` | — | `present?`, `nd_battery_snap`, `hardware`, `level`, smoothed volts, `errno` | `FuelGauge/main.c` |
| `ND_SVC_OP_BATTERY_QUICKSTART` | — | ok? | `FuelGauge/main.c` |

### Three notes on the edges of that list

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

If that page is later wanted in full, the shape is a **further op that runs a fixed list of
commands core-side and returns the parsed fields** — never a passthrough where the app
chooses the string. That keeps the surface narrow. It is not done here because it means
moving `sim_rows_present()`'s parser out of the app and into `lib/`, which is a different
work package.

**The module reset is the fifth op, and it is a write.** `ND_SVC_OP_MODEM_RESET` sends
`AT+CFUN=1,1` — it reboots the module, not the phone — for the engineering Modem app's
`*` key. It is on the wire under the same four conditions the quick-start met, and it is
worth stating them because they are the test any sixth op has to pass:

- **It carries no argument.** The app cannot choose a string; the AT command is a literal
  in `lib/` and the request record has nowhere to put an alternative. That is exactly the
  line this section draws, and it is still drawn — this is not a passthrough with one
  command in it.
- **The dangerous case is refused core-side.** A module reboot takes any live call with
  it, so `nd_modem_reset()` refuses unless the call state is idle, and it checks on the
  modem thread where that state cannot change under the check. An app cannot end a call
  through this op by accident or on purpose.
- **It adds no capability to the system.** `/etc/init.d/S45modem` already runs the
  identical command unprompted when a dial cycle fails (`MODEM_RESET_ON_FAIL`). What
  changes is who can ask for it: today a developer with a serial cable, and this is the
  engineering menu reaching the same recovery.
- **It is confirmed, in the engineering menu, on one page.** The same shape `Power` uses
  for a reboot.

What it *can* do is interrupt data — the module is gone for ~20 s and `wwan0` with it —
which is the cost the confirmation is asking about, and the reason it is not on a softkey.

The status snapshot also grew `cfun`, `cell_mode` and `rsrp_dbm10` for the same page. Both
readings are polled core-side and ride the existing `ND_SVC_OP_MODEM_STATUS` response
rather than becoming ops of their own, which is what this section asks for; see
OPEN-QUESTIONS.md M-18.

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
live; SIM is as §4 describes. RADIO's `*` key calls `nd_svc_modem_reset()`, the fifth op,
and is the only action any of the three apps has besides FuelGauge's quick-start.

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

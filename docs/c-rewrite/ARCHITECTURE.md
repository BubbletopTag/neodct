# NeoDCT in C — the architecture

*Written in plain English. If you know a bit of Python and no C, you should be able to
follow every part of this document. Where a C idea has no Python equivalent, it is
explained the first time it appears.*

---

## Why we are doing this

The phone has 64 MB of RAM. About 53 MB of that is actually usable — the rest is
carved off for the kernel and hardware buffers. Sitting on the home screen, doing
nothing, the whole system uses about **33 MB**. That leaves very little headroom, and
it is why the browser gets killed, why Koki had to grow image-cache budgets, and why
adding features feels like it costs more than it should.

The interesting part is *where* that memory goes. I measured it by booting the real UI
headlessly through the project's own `uistub.py`:

| What | Memory |
| --- | --- |
| Python interpreter, empty, nothing loaded | 8.4 MB |
| ...plus Pillow | +9.0 MB |
| ...plus sqlite3 | +2.3 MB |
| ...plus all the NeoDCT code, fonts, services, screen buffers | +1–2 MB |
| **Idle on the home screen** | **≈ 20.8 MB** |

(Those numbers are from a desktop PC, so treat them as proportions rather than exact
figures. On the phone's ARM chip everything is somewhat smaller — the UI process is
probably 15–17 MB of the 33 MB total.)

Now the number that matters most:

> **The actual picture on screen takes 123 KB.**

One 240×175 image. That is all the *real data* the UI holds while idle. Everything
else — 99.4% of it — is the machinery needed to run Python and Pillow at all. We are
not short of memory because the phone does a lot. We are short of memory because the
tools are heavy.

That is the whole case for the rewrite in one sentence: **the data is tiny, the runtime
is enormous, so replace the runtime.**

### What we expect to get back

A C version should idle around **5–9 MB** instead of 15–17 MB. Combined with a smaller
C library (see "Free wins" below), total system memory should land around **18–22 MB**
against today's 33 MB. Getting to 16 MB means trimming the Linux side too — fewer
background daemons, a smaller kernel — which is separate work we can do alongside.

### Free wins that don't need any rewriting

Two things worth knowing before we start, because they cost almost nothing:

1. **We are on glibc, not uClibc.** Neither defconfig picks a C library, and Buildroot
   defaults to glibc. Switching to musl or uClibc-ng is a config change. glibc's own
   mapping was 1.5 MB in my measurement, and its memory allocator holds onto more.
2. **Pillow on the phone is already lean.** The defconfig only enables freetype, jpeg
   and zlib — not tiff, webp, jpeg2000 or lcms. So there is nothing to trim there.

---

## The shape of the system

### Today

Everything is one Python process. Apps are loaded *into* that process with `importlib`
and run by calling `module.run(ui)`, which blocks until the app is finished. One app at
a time, like a Nokia 3310. If an app breaks, Python raises an exception, `main.py`
catches it with `except BaseException`, shows a crash screen, and returns to the menu.

That crash handling works because **Python exceptions are a language feature**. The
interpreter is watching every operation and can always stop cleanly.

### The problem with the obvious C translation

The natural instinct is: keep one process, make each app a `.so` file (a *shared
library* — the C equivalent of an importable module), and load them with `dlopen()`.

**That gives you no crash protection at all.** A `.so` loaded with `dlopen()` runs in
the same memory space as everything else. There is no interpreter watching. If an app
reads a null pointer or writes past the end of an array, the CPU raises a fault and the
kernel kills **the entire process** — core, UI, modem and all.

You can technically catch the fault (`SIGSEGV`) and jump back to the menu, and it will
often appear to work. But by then the memory is frequently already corrupted — that is
usually *why* it crashed. Memory the app allocated is leaked, locks it held stay locked,
the screen is half-drawn. It fails again later, somewhere unrelated, and you have no way
to connect the two. That is strictly worse than what you have today.

### What we do instead

Apps get their **own process**. Real memory protection, enforced by the hardware.

```
  ┌─────────────────────────────────────────┐
  │  CORE PROCESS            (never dies)   │
  │                                         │
  │   framebuffer  keypad  settings         │
  │   modem   notify   clock   battery      │
  │   the UI framework and menus            │
  └────────────────┬────────────────────────┘
                   │  fork() + execve()
                   ▼
  ┌─────────────────────────────────────────┐
  │  nd-apprun               (may die)      │
  │    dlopen("…/apps/Koki/app.so")         │
  │    call app_run()                       │
  └─────────────────────────────────────────┘
                   │
      exits cleanly ──► core redraws the menu
      crashes       ──► core draws "Koki has crashed"
```

`fork()` makes a copy of the current process. `execve()` replaces that copy with a
different program. Together they are how every Linux program starts another one — it is
what Python's `subprocess` does underneath.

`waitpid()` is how the parent finds out what happened to the child. It tells you whether
the child exited normally, and if it was killed, *which signal killed it* — so the core
knows the difference between "the user pressed Back" and "Koki dereferenced a null
pointer", and can show the right screen for each.

**Your `.so` idea survives completely.** Apps are still `.so` files sitting next to their
`manifest.json` and `icon.png`. The only change is *who* loads them: not the core, but a
small disposable stub called `nd-apprun` whose entire job is to `dlopen()` one app and
call it. The core process never has app code inside it, so no app can corrupt it —
not by accident, and not by any bug we haven't thought of.

### Why we couldn't have done this in Python

This is the part worth internalising, because it inverts one of the project's oldest
constraints.

Running an app in a separate process today would mean starting a second Python
interpreter: **+20 MB**. Completely unaffordable on a 53 MB phone. That is *why*
everything is one process.

In C, a forked-and-exec'd app costs **roughly 300–800 KB**. The UI framework, the fonts,
the font renderer and the C library are all in `libneodct.so` and the system libraries,
which are already loaded and **shared** between processes — mapped once in physical
memory, visible to everyone. The app's private cost is only the memory it actually
dirties.

So: removing Python removes the reason the design had to be single-process. We get real
crash isolation *and* use less memory than before.

### The one trap to avoid

There is a classic bug here, and it is the kind an AI agent writes without noticing.

**Never `fork()` without immediately calling `execve()` in a process that has threads
running.** When you fork a multi-threaded process, the child gets only the one thread
that called fork. If any *other* thread happened to be holding a lock at that instant,
that lock is now held forever by a thread that does not exist in the child. The child
hangs the first time it touches anything that needs that lock — including, commonly,
`malloc()`.

Our core process has threads (modem, clock). So the rule is absolute: **fork, then exec
straight away.** `nd-apprun` is a fresh program with a fresh, single-threaded state.

This will go in the coding standards for every agent working on this.

---

## Where the services go

You asked what to do with the notification and modem services. They go in the **core
process** — they are precisely the things that must outlive an app crash and be able to
interrupt a running app.

| Service | Where | How it runs |
| --- | --- | --- |
| **ModemService** | core | own thread reading the serial port |
| **NotifyService** | core | called from the core loop; owns the ringer |
| **ClockService** | core | one thread for NTP, same as today |
| **BatteryService** | core | timer, polls the i2c fuel gauge |
| **SettingsStorage / Storage** | `libneodct.so` | files and sqlite, so apps read them too |

### One thing that will change, and I want to flag it

Right now the modem is **polled from `read_keypress()`**. It is not on a thread. That
means an app only lets the modem be checked when it asks for a key — and Koki reads the
input device directly and never calls `read_keypress()`. **So an incoming call during
Koki may not interrupt properly today.**

In C the modem gets its own thread in the core process, so it works no matter what the
app is doing. When a call arrives, the core signals the app child, the child shuts down,
and the core takes the screen back.

This is a **deliberate deviation from a strict 1:1 port**. It fixes a real bug. I am
flagging it rather than quietly changing behaviour — tell me if you would rather keep
the existing behaviour exactly.

### How an incoming call interrupts an app

Today: `read_keypress()` notices the modem is ringing and raises `IncomingCall`, a
Python exception that unwinds up through the app's code back to the core loop.

In C the same shape, with processes instead of exceptions:

1. The modem thread in the core sees `RING`.
2. The core sends the app child a signal (`SIGTERM`).
3. The child's handler runs its cleanup and exits.
4. The core `waitpid()`s, takes back the screen, and shows the call UI.

This maps almost exactly onto the current behaviour, and it is *more* reliable, because
it does not depend on the app cooperating by calling `read_keypress()`.

### Sharing state between core and apps

Because apps are separate processes now, they cannot simply reach into the core's
variables. The good news is the existing code barely does this already:

- Settings already go through `SettingsStorage`, which reads and writes files.
- Contacts, messages and the call log are already in sqlite databases.
- `launch_app()` already re-reads shared state when an app finishes:
  `self._unread_sms = self._count_unread_sms()`.

That pattern is already process-friendly. Any place that *does* pass live state in
memory needs finding and converting — a real task, but a bounded one, and the surveys
are looking for exactly that.

---

## Replacing Pillow

This was the part you were most worried about, and it turned out to be the smallest
problem in the whole project.

I counted every single Pillow call in all the shipped code. Here is the complete list:

| Call | Times used |
| --- | --- |
| `draw.text` | 117 |
| `draw.rectangle` | 91 |
| `draw.line` | 29 |
| `draw.polygon` | 9 |
| `draw.ellipse` | 7 |
| `draw.textbbox` | 4 |
| `draw.point` | 2 |

Plus `Image.new`, `Image.open`, `paste`, `crop`, `resize`, `convert`, `tobytes`,
`thumbnail`, `transpose` — and two extras that only Koki uses (`point()` with a lookup
table, and `ImageChops.multiply`).

That is about **fifteen operations**. Pillow is a huge library, but this project uses a
thin sliver of it: rectangles, lines, text, and pasting one image onto another. We are
not porting Pillow. We are writing a small drawing library — the kind of thing that is
maybe 1,500 lines of C — that does exactly these fifteen things and nothing else.

Text is the only genuinely fiddly part, because the text has to land on the *same pixels*
as before. We keep FreeType (the same font renderer Pillow uses underneath) and feed it
the same `font.ttf` at the same sizes — 14, 18, 20 and 24. Same renderer, same font, same
sizes, same pixels.

---

## Proving the port is identical

You said the port must be one-to-one. That should be a test that passes or fails, not
somebody's opinion. This project already has the machinery to make that true, and it is
the single most valuable asset it has for this work.

`neodct/tools/uistub.py` runs the **real** UI with no hardware — no framebuffer, no
keypad, no modem — and captures each frame as an image. `shoot_docs.py` uses it to
produce the README screenshots, which is why they are genuine output rather than
mockups. On top of that there are **659 test functions** across 36 files.

So:

```
   Python build  ──uistub──►  frame_0001.png  ┐
                                              ├──► diff, pixel by pixel
   C build       ──dump─────►  frame_0001.png  ┘
```

Feed both builds the same key presses, dump the same screens, compare the pixels. If
they differ, the port is wrong, and the diff shows you exactly where. "One-to-one" stops
being a judgement call.

This is the reference that gets built first, before any app is ported, because every
other work package is verified against it.

---

## What this costs you

You raised losing your ability to debug, and you are right to. It is a genuine cost and
I do not want to talk you out of it.

**What gets worse.** A Python traceback tells you the file, the line, and the value of
everything on the way down. A C crash gives you a signal number and an address. Your
`CrashHandler` catches Python exceptions today; in C there is nothing to catch. You will
need `gdb` over the serial console you already have, and you will need to read stack
traces.

**What softens it.** Three things:

- **Process isolation buys back most of it.** A crash is contained to one app, the core
  survives, and the crash screen can report the signal and the faulting address. You get
  told what died and how — you just get less detail about why.
- **You already ship C and it works.** `neodctDisplay.c` is 654 lines of SPI panel
  driver with dirty-rectangle diffing, and it is the hardest hardware-facing code in the
  project. This is not new territory for the codebase.
- **QEMU still works.** The whole build runs under QEMU, where debugging is far easier
  than on the hardware, and the headless harness runs on your desktop.

**The honest summary:** debugging gets harder. Not impossible, and less bad than it
sounds because of the isolation — but harder. That is the price of the memory.

---

## Deliberately out of scope

**The browser.** It is WebKitGTK running under the `cage` compositor as a separate
process already. Nothing about it changes, and nothing about it can be improved by this
work. It stays exactly as it is.

The surveys are looking for anything else in this category. Engineering-menu tools are
the likely candidates — they are developer utilities, not phone features, and porting
them last costs nothing.

---

## Status

- [x] Architecture decided (this document)
- [ ] Ten subsystem surveys — **running now**, one agent per subsystem
- [ ] Master port plan, merged from the surveys
- [ ] Golden-frame reference captured from the Python build
- [ ] Foundation: rasterizer, font engine, `libneodct.so`, core loop
- [ ] Apps, wave by wave

The surveys write to `docs/c-rewrite/spec-*.md`. The merged plan lands in
`docs/c-rewrite/PORT-PLAN.md`.

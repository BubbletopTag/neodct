# Overnight session — status and findings

Written as the night went, so it survives if it ends early.

**Headline: Stage A is complete, green and tested against real hardware, and
both of the side quests turned out to have precise answers. One of them — the
fuel gauge — is root-caused, fixed and verified on your phone.**

---

## 1. Done and tested

Everything below was run against the actual Luckfox Pico Mini over the ethernet
debug link, not reasoned about.

### Stage A — the foundations

| piece | state |
|---|---|
| **A1 devkey channel** in `nd_input` | done, `test_devkey` 90 checks |
| **A2 `nd-key`** | done, `test_ndkey` 86 checks, wire format proved end to end |
| **A3 `nd-grab`** | done, `test_ndgrab` 75 checks, **runs on the phone** |
| **A4 `ndlink`** skeleton + verbs | done, verbs below tested on hardware |

`make test` and `make ASAN=1 test` both green, zero sanitizer findings.

### ndlink verbs that work on hardware

`devices`, `doctor`, `wait`, `shell`, `shot`, `digest`, `key`(*), `logs`,
`snapshot`, `expect`, `push`, `pull`, `connect`, `disconnect`, `state`.

All four §3 contracts were checked rather than assumed:

- `--json` on every verb, with correct escaping.
- exit codes: **1** for a failed `expect` (command worked, answer was no),
  **4** for a held lock, **5** for usage, **6** for "the phone ran it and
  failed" — each provoked deliberately and observed.
- `flock` per target, naming the holder.
- `--timeout` on everything.

Proof points worth keeping:

```
$ ndlink shot out.png          # a real 240x175 PNG of the panel, correct colours
$ ndlink state                 # version, RAM, card, backlight, battery, as JSON
$ ndlink push a b && ndlink pull b c   # byte-identical round trip over ftp
$ ndlink --json shell "exit 42"        # -> code 6
```

`nd-grab` on the phone reports, and this is the line that matters:

```
nd-grab: 240x175 32 bpp stride 960, red@0 blue@16 -> red first
```

which is the vfb's `red.offset 0` being *read* rather than assumed. The
screenshot comes back with "Eng. Mode" red, not blue.

(*) `key` on hardware needs the devkey socket, which needs an image built with
the devenv marker. The code path is complete and the wire format is verified;
it cannot run on the *currently flashed* image because that image predates all
of this. The new image built tonight has it.

---

## 2. Side quest 1: the fuel gauge — FIXED

**It was never your soldering. The gauge sleeps between reads and the first
access after idle is lost.**

The measurement that settled it, same command either way:

| | result |
|---|---|
| one read after 2 s idle, ten times | **1 succeeded, 9 failed** |
| ten reads back to back | **10 succeeded** |

The MAX1704x drops into a low-power state between accesses, and the edge that
wakes it is the one carrying its address — so that transfer is NAKed and comes
back `ENXIO`. `nd_battery_open()` runs once and reads VERSION immediately, so
its read is *by definition* the cold one, and the phone settled on
`ND_BATT_SRC_UNREADABLE` before anything else ran.

This is why it was so misleading: `i2cget` from a shell looked like proof the
hardware was fine, because only its *first* invocation was cold and the eye
goes to the nineteen that worked. Two honest readings of the same bus pointing
opposite ways.

**Fix:** three attempts a millisecond apart inside `read16()`
(`lib/nd_battery.c`). Inside `read16` and not around the probe, because every
caller wants a woken part — retrying only the probe would leave VCELL and SOC
reading through the same cold-NAK window VERSION had just walked into. A gauge
that is genuinely absent still fails all three and still settles as absent,
since `open()` classifies absence by errno, so this cannot invent a voltage.

Verified on your phone, before and after:

```
before   source unreadable   vcell null     fault "...No such device or address"
after    source live         vcell 3.8925   fault ""
         MAX1704x fuel gauge @ 0x36 on /dev/i2c-3 (VERSION=0x0003)
```

Two theories I had to discard on the way, recorded so nobody re-runs them:

- **"No device-tree node / no driver."** Wrong. There is deliberately no kernel
  driver: NeoDCT reads the gauge from userspace over `/dev/i2c-3`, and
  `settings.prop` already points at it (`system.hw.battery_i2c_bus=3`,
  `_addr=0x36`). The absence of `/sys/class/power_supply/` is expected.
- **"The transaction shape is wrong."** Wrong, and the existing comment
  defending it is right. Measured: the split write-STOP-read it uses scores
  20/20 warm; the repeated-START combined transfer scores 19/20. "Improving" it
  into `I2C_RDWR` would have made it very slightly worse.

---

## 3. Side quest 2: the keypad wizard — this one IS a broken connection

**The PCF8575 keypad expander is absent from the i2c bus.**

`nd_keypad.h:55-56` and `nd_keypadsetup.h:90-91` put the keypad on **bus 3**,
address **0x20**, probed across **0x20–0x27**. A full `i2cdetect -y -r 3` shows
**every one of those empty**. The only device on that bus is the fuel gauge at
`0x36` — which answers reliably once awake. So the bus, its pull-ups and the
SoC controller are all healthy, and it is the expander's own connection (or its
power) that is broken. That matches "I was fighting to get it closed": one chip
lost its joint, not the bus.

Everything downstream follows:

- `nd_input` looks for the matrix first, then evdev (`nd_keypad.h:361`). With no
  expander it fell back to evdev.
- The only evdev device is `adc-keys` at `event0`, whose `KEY=c0000` bitmap
  decodes to keycodes **114/115 — volume down/up only**. Stock Luckfox board
  buttons, not your sixteen keys.
- Confirmed live: nd-core (pid 2515, uid 1000) holds `/dev/input/event0` and
  `/dev/fb0` and **no i2c descriptor at all**.
- No matrix means the mapper never ran, which is why **no `keymap.json` exists**
  and the wizard never appeared.

So the sixteen-key keypad is dead and that is a physical repair.

**This makes the devkey channel far more valuable than the brief assumed.**
Once tonight's image is flashed, `ndlink key MENU` drives the phone with the
keypad hardware still broken. The repair stops being a blocker.

---

## 4. Decisions the brief did not cover

1. **Hardware became the primary backend, not QEMU.** The brief was written
   assuming no phone ("Stages A and B are fully testable without one"). You had
   one on a cable, so the testing inverted: hardware paths are *tested* and the
   QEMU paths are *written and marked unverified*. `ndlink doctor -t qemu` says
   so and must keep saying so.

2. **The QEMU image cannot be built on this workstation at all**, which forced
   the above. Buildroot's GCC 14.3.0 fails compiling its own `libcody` against
   a host GCC 15:
   `error: no matching function for call to 'S2C(const char8_t [2])'` — the
   known `u8""`-became-`char8_t` breakage. That is why `build-qemutest` had no
   toolchain. Not chased: it is a toolchain-patching job, and you have real
   hardware.

3. **`ndlink` is POSIX shell with one python helper** (`ndlink-telnet`).
   busybox `telnetd` gives an interactive pty that echoes input, prints prompts
   and frames nothing, so `printf | telnet | sed` can only decide "has it
   finished" with a fixed sleep. A tool that exists to make hardware testing
   trustworthy must not have the flakiest part be its own transport. The repo
   already ships python3 host tooling, so this adds no new dependency.

4. **The devkey channel is not a backend.** `nd_input_which()`,
   `has_matrix()` and `has_backend()` are untouched by it. `has_matrix()` gates
   the T9 indicator, which must stay off for faked keys; `has_backend()` drives
   the "this phone has no keypad" screen, and a debug channel must not talk the
   core out of showing that — especially on *this* phone, where it is true.

5. **The image built tonight is a devenv image** (`NEODCT_DEVENV_IMAGE=1`), so
   `/etc/neodct-devenv` is present and the devkey socket exists. That is
   required for `ndlink key`, and with the keypad dead it is the only way to
   drive the phone. It also enables `env.sh` sourcing. Deliberate, and yours to
   gate in the morning.

6. **`nd-grab` grew `--geom/--bpp/--stride`** for reading a raw dump that has no
   `ioctl` to ask. The brief wanted `--dev` to accept a file; this is how. It
   also means `cat /dev/fb0 > dump` over the link and converting offline works
   when the phone is too broken to run anything bigger.

---

## 5. What surprised me — the most valuable list

- **The cold-read effect on the i2c gauge.** Not in the brief, not in the code,
  and it produces two contradictory-but-honest readings of the same bus. It is
  the single most misleading hardware behaviour I hit tonight.

- **A screen with a clock is not a usable `expect` reference.** Measured, not
  guessed: two digests of the home screen a minute apart differ and `expect`
  correctly returned 1. Anything using `expect` must target clock-free screens
  or expect to re-cut constantly. This will bite the first person who snapshots
  a home screen.

- **MENU is not a physical key.** The sixteen in `nd_kpsetup_targets[]` are
  navikey, clear, up, down, 0–9, star, hash. The home screen's "Menu" label is
  NaviKey. `nd-key --list` marks LEFT, RIGHT *and* MENU as dev-keyboard-only,
  derived from that array rather than hardcoded.

- **`nd-core` logs to `/dev/ttyFIQ0`, not to a file.** `core.log` exists, is
  `root:root 0640` in an `ndusr` directory, and is written by `run_neodct.sh`,
  not by the core. So the one line that would have answered the keypad question
  immediately — "Input backend selected: ..." — goes to a serial port nobody is
  attached to. **Worth fixing separately**; it blinds exactly the debugging this
  whole exercise is about. (I first misread this as the 0.5.8b keymap.json
  permission bug repeating. It is not.)

- **A shell helper that emits a statement cannot take arguments.** My
  `remote_tool()` emitted a complete `if…fi`, so `$(remote_tool nd-grab) out.png`
  became `… fi out.png`. The failure surfaced as *the transport losing its
  framing*, three layers away from the cause.

- **A pushed binary silently loads the shipped library.** Cross-compile a fix,
  push it, run it, see no change — because rpath found `/NeoDCT/System/lib`.
  `ndlink` now sets `LD_LIBRARY_PATH=/NeoDCT/User/lib`. Without that the
  push-and-test loop quietly tests the old code.

- **`strtol` skips leading whitespace**, so `" 50 1"` would have been a second
  spelling of `"50 1"` on the devkey wire. Caught by the test, not by reading.

---

## 6. Not started

Stage B beyond `state`, `push`/`pull` and `snapshot`: `text` (T9), `apps`,
`launch`/`stop`, `settings`, `db`, `record`, `dev`, `install`/`uninstall`,
`selftest`, `bugreport`, `reboot`, `script`, `diff`. Stage C's `forward`.
Stage D entirely (the `neodct-device` skill, `backup`/`restore`, `ndlinkd`,
`watch`).

The transports they need (`hw_shell`, `hw_push`, `hw_pull`, `remote_tool`) all
exist and are tested, so most are verb bodies rather than new machinery.
`bugreport` is the one I would do next: it is the verb you wish existed the
first time something fails overnight.

---

## 7. In the morning

1. **Flash the new image** — it carries the battery fix, the devkey channel,
   `nd-key`, `nd-grab`, `nd-state`, and the devenv marker.
2. `ndlink doctor` should then show `devkey socket true`, and
   `ndlink key MENU` should move the UI **without the keypad hardware**.
3. `ndlink state` should show `"source":"live"` and a real voltage.
4. The keypad expander still needs resoldering; `0x20–0x27` on bus 3 is the
   thing to look for with a meter.
5. Gating: `nd-vncd --require-devenv` exists, and wrapping `S42debuglan`'s
   `start)` in a marker test is the one-line version for telnet/ftp/vnc.

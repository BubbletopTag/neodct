# Sleep

There is an idle mode, but no kernel suspend. This note records why it is a
deliberately shallow state and which deeper pieces do not exist yet.

## Sleep on this phone is a fake, on purpose

The obvious implementation is the kernel's: `echo mem > /sys/power/state`,
everything stops, an interrupt brings it back. That is not available here in
any form worth having.

The phone is a feature phone. Its one job while the screen is off is to ring
when somebody calls. The modem is a SIM7600G-H on a UART, and the keypad is a
PCF8575 on i2c — neither of them is a wakeup source that survives a suspend on
this SoC, and a phone that stopped answering calls when you put it in your
pocket would be a worse phone than one that never slept at all.

So "sleep" here means the phone does less, not that it stops:

1. **Downclock the CPU** to the bottom of its operating-point table.
2. **Blank the screen** — the backlight, not the framebuffer.
3. **Suspend everything that can be suspended** — the UI redraw loop, the
   fuel gauge poll, anything with a timer that nobody is looking at.
4. **Keep polling i2c and the modem**, at the slowest rate that still catches
   a keypress and a `RING` without missing them.

Steps 3 and 4 are the core's business and are not designed yet. Steps 1 and 2
are hardware pokes, and until Sleepy nobody had ever made either of them on
this board.

## What exists today

After sixty seconds without a keypad event, the core dims the panel to its
lowest lit step and pins the CPU to 600 MHz. One timer follows the physical
input path through the home screen, menus and apps; a key handled anywhere
wakes the phone and restarts it. The app process never owns a second timer.

**`Sleepy`**, in the Engineering menu, also exposes each primitive directly.
It does not schedule the automatic idle mode; it exists so the two hardware
operations can be exercised separately.

| Row | What it does |
| --- | --- |
| CPU | Lists the kernel's operating points and pins the CPU to the one you pick, then reports what the phone says it is running at afterwards. |
| CPU → `Auto (unpinned)` | Opens the range back up to the silicon's own limits, so the screen is not a door that shuts behind you. |
| Display → `BLANK!` | Turns the panel off for ten seconds and turns it back on. |
| Display → `Brightness` | Ten levels on the PWM tier. The level is remembered and re-applied at the next boot. |
| Display → `Screen off` | The same blank, held until a key is pressed. |

The decisions behind both live in `libneodct` where a test can reach them —
`nd_cpufreq.h` and the backlight half of `nd_fb.h` — and the app is the key
loop and the drawing.

## The CPU

`/sys/devices/system/cpu/cpu0/cpufreq`. On the RV1103 the table is five
entries: **408, 600, 816, 1008 and 1200 MHz**.

There is no "run at exactly this" file that works on a stock kernel.
`scaling_setspeed` exists only under the `userspace` governor and the SDK
kernel does not build it, so holding a frequency means writing the same value
to **both** `scaling_min_freq` and `scaling_max_freq`: the governor stays in
charge and has one choice.

Two consequences, both of which have bitten:

- **The write order matters.** min and max are clamped against each other, so
  the wrong order silently drops half the write and leaves the CPU where it
  was with sysfs reporting the new value on one of the two files. Raising the
  target means **max first**; lowering it means **min first**.
  `nd_cpufreq_max_first()` is that rule on its own, with its own test, because
  getting it wrong produces no error.
- **Pinning is sticky.** Nothing puts the range back when Sleepy exits. That
  is deliberate: a test whose effect vanished when you closed it could not be
  used to measure anything. It also means a phone left at 408 MHz stays there
  until something sets it again.

## The screen

`BL` is **header pin 11 — GPIO1_C5, gpio53**, also PWM9_M1. It used to be
strapped straight to 3V3; moving it onto a GPIO is what made "screen off" a
thing the software can do at all. `docs/HARDWARE_NOTES.md` has the wiring and
the two SDK patches the PWM needs.

Three tiers, best first:

| Tier | Where | What it can do |
| --- | --- | --- |
| PWM | `/sys/class/backlight/*/brightness` | Real dimming, 0–100% |
| GPIO | `/sys/class/gpio/gpio53/value` | On and off |
| none | — | Says so, honestly |

The PWM tier needs `pwm9` in the device tree, and the device tree lives in the
**boot partition** — so it arrives on a reflash, not on an update. A phone
updated over the air has the GPIO tier and only the GPIO tier. Both ship; the
fallback is not going away.

Polarity is **active high** (1 is lit), and the pin should default ON before
software runs. "No software yet" and "software broken" should both show a lit
screen, because the initramfs boot logo and the recovery sad-face are exactly
the screens you need when the rootfs is the thing that is wrong.

### The blank always ends

The one way an app can leave this phone in a state its owner cannot get out of
is by turning the screen off and not turning it back on. So Sleepy restores
the backlight on **every** path out of the blank, `app_shutdown()` included —
an incoming call during a blank arrives as `SIGTERM` (see `nd_app.h`), and a
phone that rang in the dark with no way to see who was calling would be a
worse bug than any this app was written to find.

Anything built on top of this inherits that obligation.

## Not done, and known

- **Nothing has measured the current draw.** "Downclocking saves power" is an
  assumption until somebody puts a meter on it, and 408 MHz for longer may
  well beat 1.2 GHz for less time on a workload this bursty.
- **Blanking the backlight does not stop `neodct_displayd`.** The panel daemon
  keeps pushing frames over SPI to a screen nobody can see. That is work worth
  removing before any of this counts as sleep, and it is step 3's problem.
- **No wake source is designed.** A keypress is the obvious one; whether the
  modem can raise one without a full-speed poll is the open question.
- **Sleepy is not verified on real hardware.** It was built and driven end to
  end against a scratch sysfs tree — the pin lands on both files, the panel
  goes dark and comes back, and both "nothing there" paths report honestly —
  but no Luckfox has run it. QEMU's kernel has no `CONFIG_CPU_FREQ` and no
  backlight, so on the emulator both screens correctly report that there is
  nothing there, which is the one thing the emulator *can* confirm.
- **Idle dims rather than blanks.** `Screen off` is still a row somebody
  presses. The automatic state leaves the panel legible at its lowest lit
  step so an unattended transition never looks like a dead phone.

## Why the blank did not blank

Worth recording, because the symptom was silence rather than a failure.

`nd_backlight_off()` wrote `brightness=0` and stopped. On a panel the kernel
is already holding down — `bl_power=4`, which is what
`pwm_backlight_initial_power_state()` leaves behind when the device tree gives
the backlight node a phandle — that write is stored and never acted on. The
`write(2)` succeeds, so every layer above it reported success: the library
returned true, Sleepy set `g_blanked` and sat in its ten-second loop, and the
screen stayed exactly as lit as it had been.

The `bl_power` write that fixes it was added in 0.5.9a and gated on
`percent > 0`, so it only ever ran when turning the panel **on** — the one
direction that does not need it. It is unconditional now, `0` to light and
`4` to blank, and every write is read back before it is called a success.

Two things made it expensive to find. The app named a cause instead of
reporting one, saying "Not root, or the pin is taken" about a process that
`nd_proc.c` deliberately launches as root; and the library logged one line per
process, so a blank that failed after any earlier write had already failed
printed nothing at all. Both are fixed: the dialogs repeat
`nd_backlight_last_error()`, and the log suppresses only exact repeats.

## Three faults that only showed up by looking at it

None of these could fail a unit test, and all three were in the first build
that passed every one:

- `Screen off for 10 s.` at 20 px ran off the right-hand edge of a 240 px
  panel and lost its full stop. It is 18 px now.
- The blank screen inherited the menu's softkey and offered `Select`, on a
  screen with nothing to select. It says `Wake`.
- Both "nothing there" messages were long enough that `MessageDialog`
  truncated them mid-word — `scaling_available_freque` / `ncies`. The dialog
  gives a title line and three at 14 px and it truncates rather than scrolls,
  so every message in this app is now written to that budget.

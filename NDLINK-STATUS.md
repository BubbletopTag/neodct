# Overnight session — findings and status

Written as the session went, so it survives if the night ends early.

## Side quest 1: the fuel gauge — NOT your soldering

**Verdict: the hardware is fine. The fault is in software above the i2c layer.**

Measured on the live phone over the debug LAN:

| check | result |
|---|---|
| `i2cdetect -y -r 3` | one device answers, at `0x36` |
| `VERSION` (reg `0x08`) | `0x0003` — MAX17043-family fuel gauge |
| `VCELL` (reg `0x02`) | `0xbbb0` → `0xbbb × 1.25 mV` = **3.754 V**, a real Li-ion cell |
| `SOC` (reg `0x04`) MSB | `0x09` ≈ 9% |
| static-register read reliability, as root | **29/30**, then 19/20 |
| same read **as `ndusr`** (the UI's user) | **20/20** |
| `/dev/i2c-3` | `root:i2c 0660`; `ndusr` is in group `i2c`; node is `READABLE` |

`settings.prop` already points at exactly this chip:

```
system.hw.battery_i2c_addr=0x36
system.hw.battery_i2c_bus=3
```

So NeoDCT reads the gauge from **userspace over `/dev/i2c-3`** — no kernel
driver or device-tree node is involved, and their absence (there is no
`/sys/class/power_supply/`, and no battery node in `/proc/device-tree`) is
expected rather than the bug. I chased that as a theory first and it was wrong;
recording it so nobody else spends the time.

The chip answers, the numbers decode to a plausible battery, and the user the UI
runs as can read it 20 times out of 20. **Therefore the failure is in
`nd_battery`/the UI, not in the wiring.** That is the next thing to look at, and
`nd-state` (§B2) is the verb that will show it without guessing at pixels.

## Side quest 2: the keypad wizard — this one IS a broken connection

**Verdict: the PCF8575 keypad expander is absent from the i2c bus.**

`nd_keypad.h:55-56` and `nd_keypadsetup.h:90-91` say the keypad is a **PCF8575**
on **bus 3**, default address **0x20**, probed across **0x20–0x27**.

A full `i2cdetect -y -r 3` shows **every one of `0x20`–`0x27` empty**. The only
device on that bus is the fuel gauge at `0x36` — which answers reliably. So the
bus, its pull-ups and the SoC controller are all healthy, and it is the
expander's own connection (or its power) that is broken. That matches "I was
fighting to get it closed": one chip lost its joint, not the bus.

Everything downstream follows from that:

- `nd_input` looks for the i2c matrix first, then an evdev device
  (`nd_keypad.h:361`). With no expander it fell back to evdev.
- The only evdev device is `adc-keys` at `event0`, whose `KEY=c0000` bitmap
  decodes to just keycodes **114/115 (volume down/up)** — the stock Luckfox
  board buttons, not the phone's sixteen keys.
- Confirmed live: nd-core (pid 2515, uid 1000) holds `/dev/input/event0` and
  `/dev/fb0` open and **no i2c descriptor at all**.
- No matrix means the mapper never ran, which is why **no `keymap.json` exists
  anywhere** and the wizard never appeared.

So the sixteen-key keypad is currently dead, and that is a physical repair.

**This makes the devkey channel (§A1) considerably more valuable than the brief
assumed:** once it ships, `nd-key` drives the phone over the debug LAN *without
the keypad hardware working at all*. The repair stops being a blocker.

## Incidental corrections

- `/NeoDCT/User/logs/core.log` is `root:root 0640` inside an `ndusr` directory.
  I first read that as the 0.5.8b keymap.json bug repeating. It is not:
  nd-core's stdout and stderr both point at `/dev/ttyFIQ0`, so it logs to the
  serial console and never opens that file. `run_neodct.sh` owns it. Harmless,
  but it does mean **there is no on-device log of the input-backend decision** —
  the one line that would have answered quest 2 immediately goes to a serial
  port nobody is attached to. Worth fixing separately.

## nd-vncd: confirmed working on real hardware

Shipped untested last session; verified tonight against the phone. A raw RFB
client pulled a full frame: 240×175, exactly 42000 pixels, `bpp=32` with
`R=0 G=8 B=16` as declared, 130 distinct colours, colours correct. The panel
image shows the home screen, clock, "Eng. Mode", and the battery indicator
sitting at `?` — which is what put quest 1 on the right track.

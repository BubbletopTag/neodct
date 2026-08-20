# NeoDCT OS 0.3.0a — "It's actually a phone now"

**Released:** 2026-07-26
**Targets:** QEMU aarch64 (dev) · Luckfox Pico Mini B (EVT0 hardware)

The alpha where NeoDCT stops being a UI demo and starts being a phone. It makes
and receives real calls over a SIM7600G modem, sends and receives real SMS,
shows real signal bars from a real tower — and, because the RAM budget somehow
allowed it, it browses the real web.

---

## Calls

- **Real voice calls, both directions.** Dial from the home screen and talk —
  audio goes out over the modem's USB audio channel and comes back to the
  speaker. No extra audio daemon; just ALSA plumbing.
- **The phone rings.** Incoming calls interrupt whatever you are doing with a
  3310-style call screen: caller name (looked up in your phonebook) or number,
  a flashing "calling", Answer on the softkey, C to decline.
- **Your ringtone actually plays**, looped in-process — no more 24 MB media
  player process just to ring.
- **In-call screen** shows Calling… → Ringing… → a running mm:ss timer, and
  closes itself when the other side hangs up.
- **Dial beeps** — proper DTMF tones while you type a number. Purely cosmetic,
  entirely necessary.

## Messages

- **Send SMS for real.** Write Message → Options → Send asks for a number and
  hands it to the network. Press any arrow key at the number prompt to pick a
  contact instead of typing.
- **Receive SMS for real.** A new message beeps the original Nokia tone
  instantly and puts an "N message(s) received" banner on the home screen, with
  Read on the softkey and a flashing envelope in the status bar until you read
  it. Messages that arrived while the phone was off are collected at boot.
- Opening a message finally marks it as read.
- Guardrails for empty and over-160-character drafts before anything is sent.

## Modem & signal

- **The signal bars are live** — 0–4 bars from actual tower signal strength,
  empty when unregistered.
- **The carrier name is live** — the home screen shows your operator instead of
  a permanent "No Service".
- **Auto-connect at boot**: a background service brings the modem up, checks the
  SIM, waits for LTE registration and starts a data call, retrying on its own
  when the network says no.
- **New ModemInfo engineering app** — three softkey-walked pages: RADIO
  (operator, registration, signal + dBm, call state), SIM (number, IMEI, ICCID,
  IMSI, firmware) and DATA (connection status, interface, IP, APN, DNS).
- **Simulation Mode** when there is no modem attached, so QEMU without
  passthrough still boots into a sane phone instead of erroring out.

## Web browser

- **NeoDCT has a real browser again.** NetSurf's framebuffer engine, wrapped in
  chrome drawn to match the NeoDCT UI exactly: Options menu, "Go to URL" input,
  long-text input with word wrap, softkey bar, keypad-driven cursor.
- Renders real sites over HTTPS (Wikipedia, example.com) in roughly 13 MB of
  RAM.
- The phone font is used for the UI and the built-in homepage; web pages get a
  normal readable font so real sites don't look broken.
- **Compressed swap (zram)** is enabled system-wide, sized to half of RAM —
  which is what makes browsing on a 64 MB device survivable at all.
- If the browser does die, it now says why (signal name plus a kernel log tail)
  instead of silently returning to the menu.

## Typing

- **T9 multi-tap text entry** on the physical keypad: abc / ABC / 123 modes
  cycled with #, punctuation on 1, space on 0, with a mode indicator in the
  input box.
- Fields know what they want — phonebook names are letters-only, number fields
  are digits (plus \* and #) only.
- **T9 in the Linux shell too**, via a virtual keyboard, so you can type real
  commands on the keypad.

## Music player

- Rewritten to stream audio in-process instead of spawning a media player —
  playing a song now costs kilobytes instead of ~24 MB, which was the number one
  cause of out-of-memory crashes on hardware.
- FLAC and OGG now play; progress bar and track durations are accurate.
- **Album art works**: art is picked up from a `cover.jpg` / `folder.jpg` next
  to the track, not just from tags.

## Hardware & system

- **I²C bus corrected to bus 3** across every driver after scanning the real
  board — on hardware the battery gauge and keypad were being looked for on the
  wrong bus entirely.
  *If you mapped a keypad on an earlier build, re-run KeypadMapperI2C.*
- Kernel gained everything the modem and browser need (USB serial for the modem
  AT ports, QMI network device, uinput, zram) — and it is all committed to the
  board config now, so a clean rebuild can't silently lose it.
- **Luckfox hardware caught up to QEMU**: same kernel features, same package set
  (browser, curl, uqmi, ping6, ALSA), rebuilt boot image.
- Serial console is now guarded, so a missing debug port no longer respawns a
  getty forever.

## For developers

- **Headless UI harness** (`neodct/tools/uistub.py`): boots the real UI, apps
  and framework with no framebuffer, no keypad and no device, and captures
  frames as images. Screenshot and test the whole OS without QEMU.
- **`shoot_docs.py`** renders all 55 documentation screenshots in under a
  minute.
- **113 host-side tests** covering T9, the UI framework, the browser app, the
  modem state machine and the harness itself.
- **One-command modem data proof** (`neodct/scripts/qemu_modem_data_test.py`):
  boots QEMU with the modem passed through and checks address → route → ping →
  DNS → HTTP → HTTPS.
- **Documentation site** (8 pages: how it works, stock apps, writing apps, the
  widget reference, modem, hardware, building) with three tutorial apps.
- Full technical detail for everything above is in `docs/CHANGELOG.txt` and
  `docs/MODEM_BRINGUP.md`.

---

## Known limitations

- **LTE data is not proven yet.** Voice and SMS work on air; the data call
  (`$QCRMCALL` / QMI) is still being fought with — see What's next.
- **No multitasking during a call.** An incoming call closes the running app,
  exactly like the phone this is imitating. Suspend/resume is post-alpha.
- **The browser is memory-tight.** Heavy pages can still get the browser killed
  on a 64 MB device; you'll get a crash report when it happens.
- **The root filesystem is writable**, so a bad power cut can still corrupt it.
  That is the headline item for the next release.
- Default ringtone setting points at `Brave Scottland.mp3` while the file is
  `Brave Scotland.mp3`; the fallback chain covers it, but fix the typo in
  `settings.prop` if you want your tone.

---

## What's next (0.3.1a)

**1. Make the system stable and immutable.**
Read-only root with a separate writable data partition, so the phone survives
being switched off the way phones actually get switched off — plus watchdog and
auto-restart for the UI, and bounded memory for the browser.

**2. Finish LTE internet in QEMU.**
Voice and SMS are live; the packet-data bearer is the last piece. The network is
granting the PDN and the modem still refuses to hand it over — the current lead
is stale modem session state surviving USB passthrough, with a QMI path and a
module reset as the countermeasures to prove out.

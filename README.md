# NeoDCT

NeoDCT is a custom embedded Linux device designed to fit inside a Nokia 5110 / 5190 enclosure.

## Planned Hardware

- **Compute**: Luckfox Pico Mini B (RV1103, Cortex-A7 w/ NEON, 64 MB RAM)
- **Display**: 240 × 240 ST7789 IPS LCD
- **Input**: Original Nokia keypad, wired directly to GPIO (using publicly available schematics)
- **Connectivity**: Waveshare SIM7600G-H 4G LTE modem (with GPS)
- **Audio**: IMP441 microphone, PCM5102 DAC, PAM8302 amplifier
- **Power**: 3000 mAh LiPo, USB-C charging

> **Note:** Minor faceplate modifications will be required to accommodate the taller display.  
> Finding an ST7789 panel that matches the original Nokia aspect ratio is unfortunately difficult.
>
> I've decided recently to switch the hardware target from a Radxa Zero 3W to a Luckfox Pico Mini B. The Luckfox is a MUCH smaller and more limited. In my opinion, this better fits the vibe of a dumb phone.

# NeoDCT OS

NeoDCT OS is a Linux-based, Python-driven feature phone OS inspired by classic Nokia devices—currently focused on UI and architecture, with full telephony planned next.

The UI is intentionally minimal, inspired by classic feature phones, but built on a modern Linux base using Buildroot.

---

## Screenshots

### QEMU (Development Environment)

Running NeoDCT OS in QEMU for rapid development and testing.

<p float="left">
  <img src="docs/img/qemu-mainmenu.png" width="240">
  <img src="docs/img/qemu-appselector.png" width="240">
  <img src="docs/img/qemu-linux.png" width="240">
  <img src="docs/img/qemu-snake.png" width="240">
</p>

Because NeoDCT OS is built on top of Linux, it can run more than just classic feature-phone style apps:

<p float="left">
  <img src="docs/img/qemu-mp3player.png" width="240">
</p>

---

### Real Hardware (Prototype)

> Early hardware prototype (and yes, it’s literally being held together by hand.)

```
docs/img/hardware-prototype1.jpg
docs/img/hardware-prototype2.jpg
docs/img/hardware-prototype3.jpg
```

<img src="docs/img/hardware-prototype1.jpg" width="300">
<img src="docs/img/hardware-prototype2.jpg" width="300">
<img src="docs/img/hardware-prototype3.jpg" width="300">

Currently, this device and operating system is a **work-in-progress**, not even close to a finished product.

---

## Architecture Overview

* **Kernel**: Linux (Buildroot-managed)
* **Userspace**: Minimal Linux on musl
* **UI**: `nd-core`, a custom C framework drawing straight to the framebuffer
* **Apps**: native C, one `app.so` per app, **one process each**
* **Graphics**: no desktop stack at all — no X11, no Wayland, no compositor
* **Input**: physical keypad, sixteen keys
* **Target hardware**: Luckfox Pico Mini B; QEMU for development

The UI and every app are C. The original Python implementation is kept under
`neodct/python-reference/` for comparison and is not built or shipped.

### Security at a glance

Nothing on the phone runs as root except one small broker.

* `nd-core` — the UI — runs as **`ndusr`**, an ordinary user.
* Apps run as **`ndusr`**, or as **`ndusr_ut`** if they are the browser or came
  from `/NeoDCT/User/apps`. `ndusr_ut` cannot reach the modem, your contacts,
  your messages or the signing keys — those folders are not merely unreadable
  to it, they are *absent* from its view of the filesystem.
* A **broker** process keeps root and does exactly four things: start an app,
  wait for one, halt the phone, set the clock. It refuses to run anything as
  root outside a fixed list of three programs on the read-only image.
* `/` and `/NeoDCT/System` are read-only squashfs under dm-verity.

**[`docs/HOW-IT-WORKS.md`](docs/HOW-IT-WORKS.md) explains all of it in plain
language** and is the best place to start. The design reasoning is in
`docs/c-rewrite/SECURITY.md`; the threat model it answers is in
`docs/c-rewrite/SECURITY-AUDIT.md`.

---

## Repository Layout

```
neodct/
├── buildroot/              # Vendored Buildroot source
├── neodct/
│   ├── overlay/            # Rootfs overlay (apps, UI, assets)
│   └── configs/            # Buildroot defconfigs
├── docs/
│   └── images/             # README screenshots
├── .gitignore
└── README.md
```

* `neodct/overlay/` is copied directly into the root filesystem
* No generated files or user data are tracked in git

---

## Building NeoDCT OS

### QEMU (recommended for development)

```bash
make -C buildroot \
  BR2_DEFCONFIG=../neodct/configs/neodct_qemu_defconfig defconfig

make -C buildroot
```

Produces a bootable image suitable for QEMU.

---

### Real Hardware (Luckfox Pico Mini B)

```bash
make -C buildroot O=../build-luckfox \
  BR2_DEFCONFIG=../neodct/configs/luckfox_pico_mini_defconfig defconfig

make -C buildroot O=../build-luckfox
```

Output images will appear in the corresponding `output/images/` directory.

Note that the hardware target still builds the older writable-UBIFS layout: the
read-only squashfs, dm-verity and over-the-air update system described below
currently exist on QEMU only.

---

## Project Status

NeoDCT OS is an early-stage prototype. The core UI and app framework work (including on real hardware), but most telephony features are still unimplemented.

**Legend**: 🟢 Working · 🟡 Mostly Working · 🟠 Stubbed · 🔴 Not Implemented

### 🟢 Working
- Snake
- Core Python UI framework
- Renders wallpapers and basic UI on QEMU + real hardware, ST7789 240×240

### 🟡 Mostly Working
- Phonebook (SQLite-backed; calling action is buggy)
- Web Browser (WebKitGTK via cage; QEMU-only; no video/downloads)
- Music Player (MP3 playback; browse by artist/album/song from ID3 tags, volume control)

### 🟠 Stubbed
- Messages (menu only)
- Dialer (UI only, no modem logic)
- Call Log
- ModemService (simulation mode for QEMU)
- Placeholder / test apps
- Clock (technically works, but you can't change its settings easily)

### 🔴 Not Implemented
- Telephony (calls, SMS, MMS)
- Modem integration logic in NeoDCT
- Settings, Calculator, Tones, Memory/Logic games
- Battery & signal indicators
- Clock settings
- Physical keypad support (currently external keyboard only)
- T9 / predictive text



---

## Licensing

* **NeoDCT OS code**: GPLv3
* **Linux kernel**: GPLv2
* **Buildroot**: GPLv2
* Third-party components retain their original licenses

See individual files for details.

---

## Goals

* Recreate the feel of classic Nokia feature phones (5110 / 3310)
* Modernize the concept with 4G LTE connectivity
* Add practical comfort features (GPS, MMS, etc.)
* Build on top of Linux to allow deep customization and extensibility
---

## Non-Goals

* Touch-first UX
* Heavy graphics stacks
* Doomscrollibility!! / engagement-driven UX

---

## Contributing

Contributions are welcome! I'm not the most experienced programmer! AI is the new stack overflow! :P

If you’re experimenting with NeoDCT OS and want to help out, go ahead and:

* Fork it
* Break it
* Fix it
* Send a PR

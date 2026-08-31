# A NeoDCT dev kit

A second unit on a Luckfox Pico Mini **A** -- same RV1103, same 64 MB, but
booting off a microSD card instead of SPI NAND -- with a generic 4x4 keypad,
a larger ST7789, USB ethernet and a flat breadboard layout. Not a phone.
A bench target that runs the shipping binaries on the shipping silicon and
can be reflashed by pulling a card out.

Nothing here is built. This is the reasoning, the work it would take, and
the things it would still not test, written down before any of it starts.

## 1. Why this is worth doing at all

QEMU is `-cpu cortex-a53 -m 72`, one vCPU, aarch64. The phone is a
single-core Cortex-A7 at armv7 Thumb-2 with VFPv4/NEON, musl, 64 MB, and
a panel painted over SPI by a userspace daemon. Those are not the same
machine in any respect that has ever produced a bug here.

Look at what the last few releases were actually about:

- 0.4.10a fixed red and blue being swapped, because `vfb` declares R in the
  first byte and every DRM framebuffer -- QEMU's included -- declares B.
  That bug is *invisible* on QEMU by construction.
- `neodct_displayd` v2 exists because the RV1103 has one core and a full
  115 KB frame per tick pegged it. On QEMU the frame cost is a memcpy.
- T9 -- multi-tap, the `#` mode cycle, the mode indicator -- runs only on
  the i2c matrix. `NEODCT_T9=1` makes it *visible* under QEMU; it does not
  make the scanner, the debounce or the ghosting real.
- The 0.3.10a staging bug (a 51 MiB image written to an 8 MiB partition)
  survived to a first real install because QEMU builds `userdata` at
  512 MiB.

Every one of those is a class of defect a dev kit catches and QEMU cannot.
The single most valuable property is the dullest one: **it is the only
place besides the bench phone where the armv7/musl/Thumb-2 binaries that
actually ship are executed at all.**

The second most valuable is that it is disposable. The bench unit's serial
wires break off their pads (see ROADMAP, 0.3.11a). A dev kit that boots
from a card cannot be bricked -- a bad kernel, a bad initramfs, a bad
partition table all cost thirty seconds at a card reader instead of a
maskrom session. That directly unblocks the two things currently gated on
"needs a full reflash": kernel config changes, and anything in the boot
path.

## 2. What it does not buy

Be honest about this up front, because the list is not short.

- **The NAND path.** This is the big one. The phone's storage is raw NAND
  behind MTD, UBI, `ubiblock`, and a squashfs inside a static UBI volume
  (`docs/PARTITIONS.md`). An SD card is ordinary block devices. So the dev
  kit tests dm-verity, the applier and `switch_root` over *block*, which
  is what QEMU already tests. It does not test `ubiupdatevol`, the volume
  sizing, bad-block behaviour, or the 8 MiB `userdata` that 0.3.10a is
  about. Do not treat a green dev kit as evidence about an update on NAND.
- **The modem.** SIM7600G-H is a USB device; `qmi_wwan` and the data path
  need the USB host port. `ND_SET_HW_MODEM_AT_PORT` and `MODEM_PORT` in
  `/etc/default/modem` both accept an explicit device, so a UART-attached
  SIM7600 could give AT, SMS and call control -- but not mobile data, and
  not the `qmi_wwan` framing walk in S45modem, which is where the modem
  bugs have actually been.
- **Audio, unless it is wired.** But note the phone's audio is I2S parts on
  breakout boards already (IMP441, PCM5102, PAM8302). Those are exactly as
  breadboardable as everything else here. "No USB sound" is the wrong
  framing -- the question is whether the pins are free after SPI0, i2c and
  the backlight PWM, which is a thing to count, not a thing to assume.
- **The enclosure.** No faceplate window, so the 240x175 letterbox stops
  being a physical constraint and becomes an abstract one. That is a
  hazard: it is easy to design a screen that looks fine flat on a bench
  and is wrong behind a Nokia bezel.
- **Power.** No battery, no fuel gauge unless a MAX1704x breakout is added
  (it is i2c, so it would work), no charge/discharge, nothing for the
  0.4.0a power-states work to be measured against.

## 3. The five changes the OS needs

The premise that this "creates some differences" in the OS is true, but
smaller than it looks. Most of the SD path already exists, because QEMU is
also block-based and the code was written to identify storage by content
rather than by name.

**Already works, no change:**

- `neodct/initramfs/ndsys-apply.sh` scans `/dev/mmcblk[0-9]p[0-9]` for the
  squashfs magic, and falls back to `LABEL="NDSYS"` / `LABEL="NDUSER"`.
  An SD-booted system is found by exactly the path that finds a virtio one.
- The initramfs already mounts ext4 for `/NeoDCT/User` -- the `ubiN:volume`
  spelling is the special case, not the block one.
- `neodct/scripts/post-image-neodct.sh` already emits the three images a
  card needs: `system.img` (squashfs + verity tree), `userdata.ext4`
  (labelled NDUSER, carrying `installed.prop`), and a FAT32 `sdcard.img`.

**What has to change:**

1. **A defconfig.** `neodct/configs/luckfox_pico_mini_defconfig` sets
   `BR2_ROOTFS_POST_IMAGE_SCRIPT="board/qemu/post-image.sh"` -- it does
   *not* run `post-image-neodct.sh`, so the hardware target never produces
   `system.img`, `userdata.ext4` or the initramfs. A dev-kit defconfig is
   the luckfox one with that script added and the UBI outputs dropped.
   (While in there: that file also sets
   `BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE="board/qemu/aarch64-virt/linux.config"`
   on an armv7 target. It looks vestigial -- the SDK supplies the kernel
   this board boots -- but it should be understood before being copied.)
   Remember both copies: `buildroot/configs/` is the one the build uses.

2. **A card-image script.** The dev-kit analogue of `neodct/tools/mknand.sh`:
   partition the card, `dd` the three images in, leave the SDK's
   loader/u-boot/boot region alone. Four partitions, mirroring the NAND
   table so the two stay comparable:

   | Partition | Contents |
   |---|---|
   | p1 | the SDK's boot output (FIT: kernel + built-in initramfs) |
   | p2 | `system.img` -- squashfs + verity hash tree |
   | p3 | `userdata.ext4`, LABEL `NDUSER` |
   | p4 | FAT32, LABEL `NEODCT` -- the "SD card" the UI sees |

3. **`is_system_device()` in `NeoDCT/System/hw/neodct-sdcard`.** This is the
   one real code change, and it is worth stating precisely because it is
   the only place the SD-boot design collides with a deliberate safety
   rule. That function reserves not just the system and user partitions but
   *any sibling partition on the same physical disk*:

   ```sh
   [ "$mine" = "$(parent_disk "$reserved")" ] && return 0
   ```

   On the phone that is correct and should stay correct: the card is a
   different disk from the NAND, and a squashfs found on a card is
   something someone wrote there by mistake. On the dev kit, p4 is a
   sibling of the system partition, so the card the UI is supposed to
   mount is precisely the device this rule throws away. Storage,
   MusicPlayer's card library, wallpapers, `backup_db` and the entire
   Update "look online -> download to card -> install" flow all go dark.

   The fix is not to relax the rule. It is to make the exception explicit
   -- a `neodct.card=` cmdline hint naming the partition, checked before
   the sibling test -- so the phone keeps the strict behaviour and the dev
   kit opts out by name. The alternative, a USB SD reader, costs the USB
   port (see below).

4. **`neodct_displayd` panel size.** `PANEL_W`/`PANEL_H` are `#define 240`,
   and `--yoff` already exists to place the band. A bigger panel needs
   `--panel WxH` and `--xoff` alongside it, plus whatever MADCTL the
   specific module wants. That is the whole change.

   **Do not change `ND_UI_H`.** The UI band is 240x175 and stays 240x175:
   it is in `nd_ui.h`, it is the divisor in `nd_layout.h`
   (`y = el.y * 175 / 240`), it is the size of the one long-lived canvas,
   and it is what all 48 golden frames are. A bigger panel is for *reading*
   the same 240x175 band without a magnifier -- more room around it, not
   more pixels in it. Anything else is a UI rewrite wearing a hardware
   costume, and it would also make every frame captured on the dev kit
   incomparable with the phone's.

5. **Nothing for the keypad.** This is the happy accident. The first-boot
   wizard enrols exactly sixteen keys (`nd_kpsetup_targets[]`): navikey,
   clear, up, down, 1-9, 0, `*`, `#`. A generic 4x4 matrix is sixteen
   keys. `nd_kpsetup_scan_pairs()` makes *no* row/column assumption -- it
   drives each pin, records which pins short to which, and works out the
   bipartition afterwards -- so any 8 of the PCF8575's 16 pins will do and
   the wiring order does not matter. Buy a PCF8575 breakout, wire the
   keypad's eight lines to it at i2c address 0x20 on bus 3, press each key
   once when prompted, and the shipping input path runs unmodified,
   including T9 and including the diode-less ghosting. Relabel the keypad's
   A/B/C/D column to NaviKey/C/Up/Down and it is the phone's key set
   exactly.

## 4. Ethernet, telnet and ftp

The ROADMAP records why Remote Shell is shaped the way it is:

> Idea came from LeapPad developer mode, which exposes telnet and ftpd over
> USB ethernet. The USB-C port here is charging only, so it goes over the
> internet instead.

On a dev kit that constraint is gone, and the LeapPad shape becomes
available again. Two ways to get there, and they are mutually exclusive on
a board with one USB port:

- **Device mode (RNDIS/CDC).** No extra hardware -- the cable that powers
  it is the network. Nothing else can be plugged in.
- **Host mode + a hub.** A USB ethernet dongle, and then also a USB sound
  card, a bluetooth dongle, the SIM7600, a card reader. Power then has to
  come from the 5V pad rather than the port.

Host mode is the more useful of the two by a wide margin, and it is what
makes the "no modem, no audio" caveats in section 2 negotiable. Whether
the Pico Mini's single Type-C can be put in host mode, and how, is the
first thing to establish -- it decides most of the rest of the kit.

For the shell itself: the image already builds openssh (client, server,
key utils) and `docs/REMOTE_SHELL.md` describes the whole relay design.
On a directly-reachable dev kit none of the relay is needed -- point sshd
at the USB interface instead of loopback and connect straight to it.
Adding busybox `telnetd`/`ftpd` would be less work than that sentence
makes it sound, but it would be a second, weaker access path to maintain,
and it must never reach a defconfig a phone is built from. Prefer sshd on
a non-loopback bind, guarded by the same engineering-mode gate.

## 5. Settle these before ordering anything

In rough order of how much they change the answer:

1. **USB host mode on the Pico Mini's Type-C.** Section 4. If it is
   device-only, the kit is networking-only and the modem, USB audio and
   bluetooth stay out.
2. **Free pins after SPI0 + i2c + backlight.** The display takes pins
   6, 7, 8, 11, 12, 13 (`docs/HARDWARE_NOTES.md`). The keypad takes an i2c
   pair. I2S for the IMP441/PCM5102 takes three or four more. Count them
   on the Mini A pinout before assuming audio fits.
3. **How the SDK produces an SD-boot image for the Mini A**, and where the
   kernel cmdline lives on it. On the Mini B the cmdline is in the U-Boot
   env, not the DTS -- worth confirming the SD path has not moved it,
   because `neodct.sys=`, `neodct.user=`, `neodct.verity=` and any
   `neodct.card=` all ride there.
4. **Which ST7789.** A 240x320 module is the common "bigger" one and needs
   only `--panel`/`--xoff`/`--yoff`. A 320x240 landscape module additionally
   needs a MADCTL rotation. Pick the portrait one.
5. **`CONFIG_INITRAMFS_SOURCE`.** The initramfs is built *into* the kernel
   on this board because the FIT image the standard path produces has no
   ramdisk slot. That stays true on SD, so an initramfs change still means
   a kernel rebuild -- but it is now a rebuild you install with a card
   reader, which is the entire point of the exercise.

## 6. The verdict

Yes, and it is better than QEMU for the things QEMU is worst at: the real
instruction set, the real memory ceiling, the real panel, the real
keypad, and one core. It is worse than the bench phone for storage, and
it tests nothing at all about NAND, UBI or the enclosure.

The honest positioning is a third rung, not a replacement for either:

```
host tests   ->  510 pytest + the C suite + 48 golden frames  (seconds)
QEMU         ->  the whole OS, wrong CPU, wrong panel, no keypad  (a minute)
dev kit      ->  right CPU, right panel, right keypad, wrong storage
bench phone  ->  everything, and a soldering iron when it goes wrong
```

The work is roughly: one defconfig, one card-image script, one cmdline
hint plus the `is_system_device()` exception it enables, and two flags in
`neodct_displayd`. The keypad, the initramfs and the image build need
nothing. That is a small enough pile to be worth it for the reflash cycle
alone.

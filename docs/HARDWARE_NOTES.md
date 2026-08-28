# Luckfox Pico Mini B hardware bring-up notes

## Wiring (ST7789 240x240)
CS   -> pin 6  (SPI0_CS0_M0)
CLK  -> pin 7  (SPI0_CLK_M0)
SDA  -> pin 8  (SPI0_MOSI_M0)  -- NOT pin 9 (MISO)!
RST  -> pin 12 (GPIO1_D0 / gpio56)
DC   -> pin 13 (GPIO1_D1 / gpio57)
BL   -> pin 11 (GPIO1_C5_d / PWM9_M1 / gpio53)
        Was 3V3 direct. On a GPIO now so the screen can be switched off,
        which is most of what a backlight costs. Dimming needs pwm9 in the
        device tree -- that lives in the boot partition, so it arrives on
        a reflash, not an update. System/hw/backlight.py uses the PWM when
        it is there and falls back to on/off through /sys/class/gpio,
        which is the same interface neodct_displayd uses for RST and DC.

        Pin state before software runs decides what a failed boot looks
        like. Default it ON (pull-up on the enable) so "no software yet"
        and "software broken" both show a lit screen -- the initramfs
        draws the boot logo and the recovery sad-face, and those are the
        screens you need when the rootfs is the thing that is wrong.

## SDK patches for the backlight (not in this repo -- reapply after a re-clone)

The wire is only half of it. Two files in `luckfox-pico/` have to change,
and that tree is not version controlled here, so this is the record.
Backups are written alongside as `*.bak-neodct-<date>`.

**1. `sysdrv/source/kernel/arch/arm/boot/dts/rv1103g-luckfox-pico-mini.dts`**

In the root node:

```dts
backlight: backlight {
    compatible = "pwm-backlight";
    pwms = <&pwm9 0 25000 0>;
    brightness-levels = <0 1 2 4 8 16 32 48 64 80 100>;
    default-brightness-level = <10>;
    status = "okay";
};
```

and at the end, beside the other PWM overrides:

```dts
&pwm9 {
    pinctrl-names = "active";
    pinctrl-0 = <&pwm9m1_pins>;
    status = "okay";
};
```

`"active"`, not `"default"` -- that is the state name the rockchip pwm
driver looks for, and what the `pwm9` node in `rv1106.dtsi` declares.
PWM9_M1 shares pin 11 with UART4_TX_M1, which this board file already
disables, so nothing collides.

**2. `sysdrv/source/kernel/arch/arm/configs/luckfox_rv1106_linux_defconfig`**

```
CONFIG_BACKLIGHT_CLASS_DEVICE=y
CONFIG_BACKLIGHT_PWM=y        # was =m
```

It has to be built in. The stock defconfig has `CONFIG_BACKLIGHT_PWM=m`,
and the SDK kernel's modules never reach this rootfs -- buildroot builds
the rootfs, and its `/lib/modules` is a different kernel version
entirely. A module here is a file that does not exist on the phone.

`CONFIG_PWM=y` and `CONFIG_PWM_ROCKCHIP=y` are already set upstream.

### Rebuilding and flashing just this

A device tree change only touches the boot partition, so there is no need
to rewrite the whole device:

```sh
# regenerate the initramfs first -- build.sh bakes in whatever
# CONFIG_INITRAMFS_SOURCE points at, and luckfox's make never rebuilds it
neodct/scripts/mkinitramfs.py --target-dir build-luckfox/target \
    --init neodct/initramfs --output build-luckfox/images/initramfs.cpio.gz
gzip -dc build-luckfox/images/initramfs.cpio.gz > build-luckfox/images/initramfs.cpio

distrobox enter luckfox -- bash -lc 'cd luckfox-pico && ./build.sh kernel'
cd ../luckfox-pico && sudo ./rkflash.sh boot
```

`rkflash.sh boot` runs `upgrade_tool di -b boot.img` and nothing else, so
rootfs and userdata survive -- settings, contacts and the Remote Shell
keys all stay put. `rkflash.sh update` would rewrite every partition and
take them with it.

Afterwards `/sys/class/backlight/backlight/` should exist.
`System/hw/backlight.py` finds it on its own and moves from on/off to
real dimming with no change to the rootfs.

## Display driver: userspace, not fbtft
fbtft (kernel driver) never worked on 5.10 despite correct DT + wiring -
root cause never fully isolated (suspect: 5.10 gpiod reset polarity bug).
Abandoned in favor of neodct_displayd (src/neodct_displayd.c), which
drives the panel directly over /dev/spidev0.0 and mirrors a Linux
virtual framebuffer (vfb) to it. Proven working.

### vfb puts red in the FIRST byte, unlike almost everything else
At 32bpp `vfb_check_var()` declares red.offset 0, green 8, blue 16,
transp 24 - so a pixel in /dev/fb0 is bytes R G B A, not the B G R x that
a DRM framebuffer (QEMU's, and every desktop) gives you. See
`drivers/video/fbdev/vfb.c`. At 16bpp it is likewise blue-first: red 0,
green 5, blue 11.

This is not a detail you can skip. The UI and the daemon used to assume
B G R x on both targets; being wrong together, they looked right, and
every program that read the declaration instead - mpv, netsurf, the
framebuffer console - drew into fb0 in an order the daemon then undid.
Both ends read fb_var_screeninfo now. If you write anything new that
touches fb0 directly, read the offsets; do not assume a depth implies an
order.

## Build gotchas
- SDK build MUST happen in Ubuntu 22.04 (distrobox). Native Arch hangs
  silently on the atbm wifi driver.
- neodct's own Buildroot tree builds fine natively on Arch.
- vfb is disabled by default even when CONFIG_FB_VIRTUAL=y and built-in;
  vfb_setup() stomps the enable flag at boot regardless of cmdline.
  Patched drivers/video/fbdev/vfb.c line ~399 to force-enable.
  See patches/luckfox-sdk/0001-vfb-force-enable.patch
- Real kernel cmdline comes from U-Boot env (.env.txt in SDK output),
  NOT from the DTS bootargs node. DTS bootargs are effectively ignored
  on this board.
- Flash rootfs only, without touching boot partition:
  sudo ./upgrade_tool di -rootfs rootfs.ubifs
## Flashing a NeoDCT rootfs

The SDK flashes whatever is in `luckfox-pico/output/image/`. NeoDCT's own
Buildroot produces the UBI image; the link between them is a copy:

```sh
# 1. build the luckfox rootfs (build-luckfox/ is stale -- it points at a
#    /home/bubbles/Documents path that no longer exists; use a fresh O=)
make -C buildroot O=../build-lf \
  BR2_DEFCONFIG=../neodct/configs/luckfox_pico_mini_defconfig defconfig
make -C buildroot O=../build-lf

# 2. hand it to the SDK
cp ../build-lf/images/rootfs.ubi ../luckfox-pico/output/image/rootfs.img

# 3. flash just that partition (device in maskrom/loader mode)
cd ../luckfox-pico && sudo ./rkflash.sh rootfs
```

`rkflash.sh` with no argument flashes everything (loader, uboot, boot, oem,
userdata, rootfs) and will wipe user data. `rkflash.sh rootfs` is the one to
use day to day. `sudo ./upgrade_tool di -rootfs rootfs.ubifs` is the
lower-level equivalent.

## Kernel cmdline lives in the U-Boot env, not the DTS

`output/image/env.img` carries it, and the DTS `bootargs` node is ignored on
this board:

```
sys_bootargs= ubi.mtd=6 root=ubi0:rootfs rootfstype=ubifs rk_dma_heap_cma=24M
```

`output/image/.env.txt` holds the matching `mtdparts=` line. Anything the
initramfs reads off the cmdline goes here -- `neodct.rectty=/dev/console` to
drive recovery over the serial console, and eventually `neodct.sys=`,
`neodct.user=` and `neodct.verity=` when the immutable layout reaches
hardware.

## SDK kernel: dm-verity added for the immutable layout

The stock luckfox kernel has no device-mapper at all, so dm-verity could not
run. Added to `sysdrv/source/kernel/arch/arm/configs/luckfox_rv1106_linux_defconfig`
(a `.bak-YYYYMMDD` copy sits beside it):

```
CONFIG_MD=y
CONFIG_BLK_DEV_DM=y
CONFIG_DM_VERITY=y
CONFIG_CRYPTO_SHA256=y
```

Rebuild inside the Ubuntu 22.04 container, which is not optional -- a native
Arch build hangs on the atbm wifi driver:

```sh
distrobox enter luckfox -- bash -lc 'cd /path/to/luckfox-pico && ./build.sh kernel'
```

Already present and needed by the same layout: `CONFIG_MTD_UBI_BLOCK`
(exposes a UBI volume as `/dev/ubiblockN_M`, the block device squashfs and
verity both require) and `CONFIG_SQUASHFS_XZ`. Note there is **no**
`CONFIG_SQUASHFS_ZSTD`, so the luckfox defconfig must build squashfs with XZ
while qemu uses zstd.

## SDK kernel: Bluetooth -- NOT DONE, and the 5.10 table is wrong for this dongle

**Nothing here has been built or run.** The Luckfox SDK tree that produces
this kernel is `~/Documents/Projects/luckfox-pico`, and it is **not on this
machine** -- the directory does not exist, and the box has 17 GB free, so
re-cloning it was not attempted either. What follows is what a build would
need, derived from the 5.10 sources themselves rather than from memory, and
it has to be treated as a proposal until someone runs it.

The rootfs half IS done and needs nothing further: the Realtek firmware ships
in `neodct/overlay/lib/firmware/rtl_bt/`, and both defconfigs use that same
overlay. A kernel with no `CONFIG_BT` makes it moot, which is the whole of
what is missing.

### The defconfig fragment

`sysdrv/source/kernel/arch/arm/configs/luckfox_rv1106_linux_defconfig`, the
same file the backlight and dm-verity notes above edit (keep the
`.bak-YYYYMMDD` habit):

```
CONFIG_BT=y
CONFIG_BT_BREDR=y
CONFIG_BT_HCIBTUSB=y
CONFIG_BT_HCIBTUSB_RTL=y
CONFIG_FW_LOADER=y
```

`=y` and not `=m` for the reason the backlight note gives: the SDK kernel's
modules never reach this rootfs, because buildroot builds the rootfs and its
`/lib/modules` belongs to a different kernel version entirely. A module here
is a file that does not exist on the phone.

No `CONFIG_BT_HCIVHCI` -- that is a QEMU test hook and has no business on
hardware. No `CONFIG_RFKILL` unless a radio switch is ever wired.

### The part that will actually bite: 5.10's btrtl asks for the wrong firmware

This was checked against the real `linux-5.10.110` source, not assumed.

The UB500 is an RTL8761BU: LMP subversion `0x8761`, HCI revision `0xb`, on
`HCI_USB`. In **6.12** `drivers/bluetooth/btrtl.c` has two entries for that
combination and tells them apart by bus:

```c
{ IC_INFO(RTL_ROM_LMP_8761A, 0xb, 0xa, HCI_UART), .fw_name = "rtl_bt/rtl8761b_fw",  ... }
{ IC_INFO(RTL_ROM_LMP_8761A, 0xb, 0xa, HCI_USB),  .fw_name = "rtl_bt/rtl8761bu_fw", ... }
```

In **5.10** there is only one, and it does not look at the bus at all:

```c
	/* 8761B */
	{ IC_INFO(RTL_ROM_LMP_8761A, 0xb),
	  .config_needed = false,
	  .has_rom_version = true,
	  .fw_name  = "rtl_bt/rtl8761b_fw.bin",
	  .cfg_name = "rtl_bt/rtl8761b_config" },
```

So a 5.10 kernel matches the USB dongle against the **UART** part and asks
for `rtl8761b_fw.bin`, which is a different image from `rtl8761bu_fw.bin`
(45,444 bytes against 44,484). Shipping the correct firmware is not enough:
the kernel will not ask for it by that name.

Two ways out, and the second is the honest one.

**(a) Rename.** Ship `rtl8761bu_fw.bin` as `rtl8761b_fw.bin`. It works only
because the 5.10 entry is bus-agnostic, and it silently breaks the day the
kernel is updated or a genuine 8761BTV is attached. Fine for a bring-up
afternoon, wrong to commit.

**(b) Backport the split.** 5.10's `struct id_table` already has an
`hci_bus` field and `btrtl_match_ic()` already honours `IC_MATCH_FL_HCIBUS`
-- three other entries use it -- so this is data, not logic. Replace the
single 8761B entry with:

```c
	/* 8761BTV (UART) */
	{ .match_flags = IC_MATCH_FL_LMPSUBV | IC_MATCH_FL_HCIREV |
			 IC_MATCH_FL_HCIBUS,
	  .lmp_subver = RTL_ROM_LMP_8761A,
	  .hci_rev = 0xb,
	  .hci_bus = HCI_UART,
	  .config_needed = false,
	  .has_rom_version = true,
	  .fw_name  = "rtl_bt/rtl8761b_fw.bin",
	  .cfg_name = "rtl_bt/rtl8761b_config" },

	/* 8761BU (USB) */
	{ .match_flags = IC_MATCH_FL_LMPSUBV | IC_MATCH_FL_HCIREV |
			 IC_MATCH_FL_HCIBUS,
	  .lmp_subver = RTL_ROM_LMP_8761A,
	  .hci_rev = 0xb,
	  .hci_bus = HCI_USB,
	  .config_needed = false,
	  .has_rom_version = true,
	  .fw_name  = "rtl_bt/rtl8761bu_fw.bin",
	  .cfg_name = "rtl_bt/rtl8761bu_config" },
```

Note the naming convention differs between the two kernels and the 5.10 one
is what matters here: `fw_name` carries the `.bin` extension and `cfg_name`
does not, because `btrtl_initialize()` appends `"%s.bin"` to the config name
itself. Copying 6.12's strings verbatim would ask for `rtl8761bu_fw` and
`rtl8761bu_config.bin.bin`.

### How to tell which one happened, without a serial cable

Open the Bluetooth engineering app (id 9007) and run Self test. The five steps
separate every one of these failures:

* `KERNEL FAIL` -- the defconfig fragment above did not take.
* `ADAPTER FAIL` -- `CONFIG_BT_HCIBTUSB` missing, or nothing on the USB port.
* `ADDRESS FAIL`, BD_ADDR all zeros -- the firmware did not load. On a 5.10
  kernel with the correct files installed, **this is the expected result**,
  and it is the signature of the wrong-name problem above.
* `RADIO FAIL` -- the controller is there and does not answer.

### The hardware question nobody has answered

The Pico Mini B has **one** USB port and the SIM7600 is on it. A dongle and a
modem at the same time needs a hub. The UB500's descriptor asks for 500 mA
(`MaxPower 500mA`, self-powered), on a board that is itself powered over that
same USB, so the power budget wants measuring before any of this is trusted
on a bench, let alone in a case.

## NAND images

`neodct/tools/mknand.sh <images-dir> <target-dir> [host-dir]` turns a finished
luckfox build into flashable images: `system.ubi` (the squashfs + verity tree
as a UBI static volume) and `userdata.ubi` (an empty ubifs volume). Geometry
matches the chip -- 128KiB erase blocks, 2048-byte pages and sub-pages -- and
it refuses to emit anything larger than its partition.

There is no `ubiattach`, `ubiblock` or `ubinize` in the target, so attaching
is the kernel's job from the cmdline: `ubi.mtd=` for each partition and
`ubi.block=` to expose the system volume.

## The immutable layout on NAND

The stock table wasted 30M on an `oem` partition that was never mounted, and
left only 4M for boot -- too little for a kernel plus the 1.65M initramfs.
That 30M is split three ways: boot +12M, rootfs +15M, userdata +2M. That is
29M, so the new table uses 1M *less* of the chip than the old one, which
becomes extra bad-block slack.

`docs/PARTITIONS.md` describes the whole layout in Simplified Technical
English, with a per-partition delta table. New table
(`RK_PARTITION_CMD_IN_ENV` in the board config):

```
256K(env),256K@256K(idblock),512K(uboot),16M(boot),8M(userdata),100M(rootfs)
   mtd0     mtd1              mtd2         mtd3      mtd4          mtd5
```

The initramfs is built **into the kernel** (`CONFIG_INITRAMFS_SOURCE`) rather
than carried as a FIT ramdisk: the boot image the standard path builds has no
ramdisk slot, and embedding avoids rebuilding that tooling. boot.img is 5.27M
of the 16M partition. The path in the defconfig is absolute, so it points at
whichever build tree produced `initramfs.cpio` -- regenerate it before
rebuilding the kernel.

Extra kernel arguments come from `NEODCT_BOOTARGS_EXTRA` in the board config,
appended by `__GET_BOOTARGS_FROM_BOARD_CFG` in `project/build.sh`.

**It must not be called `RK_*`.** `unset_env_config_rk()` at the top of
build.sh clears RK variables with `env | grep -oh "^RK_.*=" | source`, and
that `.*` is greedy: a value containing `=` -- which every kernel argument
has -- becomes a malformed line that is then sourced, and the build dies with
`unexpected EOF while looking for matching '"'` before it prints anything.

Resulting cmdline:

```
ubi.mtd=5 root=ubi0:rootfs rootfstype=ubifs rk_dma_heap_cma=1M
ubi.mtd=4 ubi.block=0,system neodct.sys=/dev/ubiblock0_0
neodct.user=ubi1:userdata neodct.verity=enforce neodct.rectty=/dev/console
```

mtd5 attaches first so the system volume is ubi0 and `ubi.block=0,system`
exposes it as `/dev/ubiblock0_0`; userdata follows as ubi1. The generated
`root=` is ignored because an initramfs `/init` exists. Note CMA drops from
24M to 1M, which hands ~23M of a 64M phone back to the system.

### Building and flashing it

```sh
make -C buildroot O=../build-lf BR2_DEFCONFIG=.../luckfox_pico_mini_defconfig defconfig
make -C buildroot O=../build-lf

# Build the initramfs. This is not optional and nothing else does it: the
# luckfox defconfig's post-image hook is board/qemu/post-image.sh alone, so
# `make` leaves whatever initramfs.cpio.gz was there last time. Skip this
# and you flash a fresh rootfs under a stale boot path -- which is how you
# end up with an update applier that does not match the system it installs.
neodct/scripts/mkinitramfs.py --target-dir ../build-lf/target \
    --init neodct/initramfs --output ../build-lf/images/initramfs.cpio.gz

neodct/tools/mknand.sh ../build-lf/images ../build-lf/target ../build-lf/host
gzip -dc ../build-lf/images/initramfs.cpio.gz > ../build-lf/images/initramfs.cpio

cp ../build-lf/images/system.ubi   luckfox-pico/output/image/rootfs.img
cp ../build-lf/images/userdata.ubi luckfox-pico/output/image/userdata.img
distrobox enter luckfox -- bash -lc 'cd luckfox-pico && ./build.sh kernel && ./build.sh env && ./build.sh updateimg'
cd luckfox-pico && sudo ./rkflash.sh update      # device in maskrom mode
```

`rkflash.sh` has no `env` target and its default "all" matches no branch and
does nothing, so a partition-table change has to go through `update`
(`upgrade_tool uf update.img`), which rewrites the table and every partition.

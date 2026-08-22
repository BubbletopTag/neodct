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

## Display driver: userspace, not fbtft
fbtft (kernel driver) never worked on 5.10 despite correct DT + wiring -
root cause never fully isolated (suspect: 5.10 gpiod reset polarity bug).
Abandoned in favor of neodct_displayd (src/neodct_displayd.c), which
drives the panel directly over /dev/spidev0.0 and mirrors a Linux
virtual framebuffer (vfb) to it. Proven working.

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

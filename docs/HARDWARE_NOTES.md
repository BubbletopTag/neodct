# Luckfox Pico Mini B hardware bring-up notes

## Wiring (ST7789 240x240)
CS   -> pin 6  (SPI0_CS0_M0)
CLK  -> pin 7  (SPI0_CLK_M0)
SDA  -> pin 8  (SPI0_MOSI_M0)  -- NOT pin 9 (MISO)!
RST  -> pin 12 (GPIO1_D0 / gpio56)
DC   -> pin 13 (GPIO1_D1 / gpio57)
BL   -> 3.3V direct

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

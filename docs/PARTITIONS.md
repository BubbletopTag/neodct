# NAND partitions on the Luckfox Pico Mini B

Mode: STE-flavored for sections 1 to 9, Strict for section 10 (the flashing
procedure).

## 1. Scope

The Luckfox Pico Mini B carries a 128 MiB SPI NAND chip. The chip is raw NAND
behind MTD. This document gives the NeoDCT partition table for that chip. It
also gives the kernel arguments that attach each partition, the boot sequence,
and the flashing procedure. Section 11 defines the terms.

## 2. The layout

The partition table lives in the board config as `RK_PARTITION_CMD_IN_ENV`:

```
256K(env),256K@256K(idblock),512K(uboot),16M(boot),8M(userdata),100M(rootfs)
```

| Device | Name | Size | Contents |
|---|---|---|---|
| `mtd0` | env | 256 KiB | The U-Boot environment. It holds the kernel cmdline. |
| `mtd1` | idblock | 256 KiB | Vendor boot data. It starts at offset 256 KiB. |
| `mtd2` | uboot | 512 KiB | The U-Boot bootloader. |
| `mtd3` | boot | 16 MiB | One FIT image. The kernel and the initramfs are in it. |
| `mtd4` | userdata | 8 MiB | A UBI volume named `userdata`. It holds UBIFS. |
| `mtd5` | rootfs | 100 MiB | A UBI volume named `system`. It holds a squashfs and a hash tree. |

The six partitions use 125 MiB of the chip. The other 3 MiB is slack for bad
blocks.

The `env`, `idblock` and `uboot` partitions keep the names, the sizes and the
offsets of the old table. Only the last three partitions changed.

## 3. What changed from the old layout

The old table was:

```
256K(env),256K@256K(idblock),512K(uboot),4M(boot),30M(oem),6M(userdata),85M(rootfs)
```

| Partition | Old size | New size | Change |
|---|---|---|---|
| env | 256 KiB | 256 KiB | none |
| idblock | 256 KiB | 256 KiB | none |
| uboot | 512 KiB | 512 KiB | none |
| boot | 4 MiB | 16 MiB | +12 MiB |
| oem | 30 MiB | removed | -30 MiB |
| userdata | 6 MiB | 8 MiB | +2 MiB |
| rootfs | 85 MiB | 100 MiB | +15 MiB |

The `oem` partition is gone. Nothing mounted it. The directory `/oem` does not
exist on the device. The 30 MiB from `oem` is now in `boot`, `userdata` and
`rootfs`. The new table uses 1 MiB less of the chip than the old table.

The old `rootfs` partition held UBIFS, and the kernel mounted it read-write.
The old cmdline attached it with `ubi.mtd=6`. It then mounted that volume as
`root=ubi0:rootfs`. `/NeoDCT/User` was an ordinary directory on that writable
root. The filesystem was 98% full. It used 70.5 MiB of 71.6 MiB.

## 4. What each partition holds

### boot (16 MiB)

The `boot` partition holds one FIT image of 5.27 MiB. The image holds the
kernel. `CONFIG_INITRAMFS_SOURCE` puts the initramfs inside that kernel. The
initramfs is 1.65 MiB. The initramfs is inside the kernel because the FIT image
that the standard build path produces has no ramdisk slot.

### userdata (8 MiB)

The `userdata` partition holds a UBI volume named `userdata`. The volume holds
UBIFS. The initramfs mounts that volume and moves it to `/NeoDCT/User`. That
volume is the only writable storage on the device.

### rootfs (100 MiB)

The `rootfs` partition holds `system.ubi`. That file is a static UBI volume
named `system`. The volume holds a squashfs of 49.9 MiB. The build appends
a dm-verity hash tree to that squashfs. The full image is 52.25 MiB.

Note the two names. The partition is `rootfs`. The volume inside it is
`system`.

`neodct/tools/mknand.sh` produces `system.ubi` and `userdata.ubi`. It refuses
to write an image that is larger than its partition.

## 5. Why UBI and ubiblock

The chip is raw NAND behind MTD, so the device has no block devices at all.
UBI adds wear levelling and bad-block handling on top of MTD.
`CONFIG_MTD_UBI_BLOCK` then presents a UBI volume as `/dev/ubiblockN_M`, which
is a block device. squashfs needs a block device. dm-verity needs one too.
This is why the system image is in a UBI volume and not directly on MTD.

The target has no `ubiattach`, no `ubiblock` and no `ubinize`. The kernel
attaches the volumes from the cmdline instead.

## 6. The kernel cmdline

The build generates the cmdline, and `env.img` carries it. This board ignores
the `bootargs` node in the DTS.

```
ubi.mtd=5 root=ubi0:rootfs rootfstype=ubifs rk_dma_heap_cma=1M
ubi.mtd=4 ubi.block=0,system neodct.sys=/dev/ubiblock0_0
neodct.user=ubi1:userdata neodct.verity=enforce neodct.rectty=/dev/console
```

| Argument | Function |
|---|---|
| `ubi.mtd=5` | Attaches mtd5, the `rootfs` partition, as the first UBI device. |
| `root=ubi0:rootfs` | Unused. See the note below this table. |
| `rootfstype=ubifs` | Unused. See the note below this table. |
| `rk_dma_heap_cma=1M` | Reserves 1 MiB for CMA. |
| `ubi.mtd=4` | Attaches mtd4, the `userdata` partition, as the second UBI device. |
| `ubi.block=0,system` | Presents the `system` volume on ubi0 as `/dev/ubiblock0_0`. |
| `neodct.sys=/dev/ubiblock0_0` | Tells the initramfs where the system image is. |
| `neodct.user=ubi1:userdata` | Tells the initramfs where the user partition is. |
| `neodct.verity=enforce` | Makes the initramfs stop the boot if verification fails. |
| `neodct.rectty=/dev/console` | Puts recovery on the serial console. |

The build generates `root=` and `rootfstype=`. The kernel does not use either
one, because an initramfs `/init` exists. The initramfs finds the root through
`neodct.sys=`.

CMA drops from 24 MiB to 1 MiB. That returns about 23 MiB to a device with
64 MiB of RAM.

## 7. Volume numbers and attach order

UBI numbers its devices in the order the kernel attaches them. The cmdline
gives `ubi.mtd=5` before `ubi.mtd=4`, so the numbers follow that order and not
the MTD numbers:

1. mtd5, the `rootfs` partition, attaches first. It becomes ubi0. The `system`
   volume is therefore on ubi0.
2. mtd4, the `userdata` partition, attaches second. It becomes ubi1. The
   `userdata` volume is therefore on ubi1.

`ubi.block=0,system` names ubi0 and the volume `system`, and the kernel makes
the block device `/dev/ubiblock0_0`. `neodct.user=ubi1:userdata` names the
volume on ubi1.

The two `ubi.mtd=` arguments therefore set the numbers in `neodct.sys=` and
`neodct.user=`. If you change the order of the two arguments, you must change
those two values as well.

## 8. The boot sequence

`neodct/initramfs/init` runs before the kernel mounts any root filesystem. It
does these steps in this order:

1. It mounts `/proc`, `/sys` and `/dev`.
2. It reads the kernel cmdline for the device names.
3. It shows the boot logo on the SPI panel.
4. It mounts the user partition read-write.
5. It applies a staged update, if one is present.
6. It builds a dm-verity device over the system partition.
7. It mounts the system image read-only.
8. It moves the user partition into the new root.
9. It runs `switch_root`.

Step 5 must happen here. This is the only safe moment to rewrite the system
partition. The kernel does not page a squashfs from it yet.

## 9. What an update replaces

An `UPDATE.ndsw` package carries a full `rootfs.squashfs`. An update therefore
replaces the whole system partition. Only `/NeoDCT/User` survives an update.

The package carries no kernel. A change to the kernel or to the kernel config
cannot travel in an update. Such a change needs a full reflash.

## 10. How to flash the layout

Warning: this procedure writes the partition table and every partition. The
`userdata` partition is one of them, so the user data does not survive.

Do these steps:

1. Put the device in maskrom mode.
2. Run these two commands:

```sh
cd luckfox-pico
sudo ./rkflash.sh update
```

The `update` target runs `upgrade_tool uf update.img`. That tool writes the
partition table. It then writes every partition.

`rkflash.sh` has two limits:

- It has no `env` target.
- Its default argument `all` matches no branch. The script then does nothing.

Use the `update` target for every change to the partition table.

`docs/HARDWARE_NOTES.md` gives the build steps that produce `update.img`.

## 11. Terms

| Term | Meaning |
|---|---|
| MTD | The kernel layer for raw flash chips. It provides no block devices. |
| UBI | A layer above MTD. It adds wear levelling and bad-block handling. |
| UBIFS | A read-write filesystem for a UBI volume. |
| ubiblock | The kernel feature that presents a UBI volume as a block device. `CONFIG_MTD_UBI_BLOCK` enables it. |
| static volume | A UBI volume whose size is the size of the image in it. |
| FIT image | The U-Boot image format that the `boot` partition uses. |
| squashfs | A read-only compressed filesystem. |
| dm-verity | A device-mapper target. It checks each block against a hash tree. |
| CMA | Contiguous memory that the kernel reserves at boot for drivers. |
| maskrom mode | The Rockchip boot mode in which the host tool can write to the chip. |

# Flashing NeoDCT onto the phone

This is the "start from nothing" procedure. It rewrites the whole chip:
partition table, bootloader, kernel and system. Use it when the phone will
not boot, or when the partition layout changed.

**It erases everything on the phone** -- contacts, messages, settings, the
Remote Shell keys, everything in User. There is no way to keep them; that is
what makes it able to fix a phone that is broken.

Two paths in this document, and you almost certainly want the first one:

- **Part A** -- flash the `update.img` that is already built. Five minutes.
- **Part B** -- build a new `update.img` from the source tree first. An hour,
  mostly waiting.

Throughout, `SDK` means the Luckfox SDK folder:

```
/run/media/bubbles/ff277fc3-600e-4010-b73e-76a24c99656c/Projects/luckfox-pico
```

---

## Part A -- flash it

### Step 0. One time only, on a computer that has never flashed this board

Teach Linux to let you talk to the chip without being root all the way down:

```sh
cd /run/media/bubbles/ff277fc3-600e-4010-b73e-76a24c99656c/Projects/luckfox-pico
sudo cp tools/linux/Linux_Upgrade_Tool/88-rockusb.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

Skip this if you have flashed this board from this computer before.

### Step 1. Put the phone in maskrom mode

Maskrom is the mode where the chip is a dumb USB device waiting to be
written to. It is built into the silicon, so it works even when everything
else on the phone is broken. That is why a bricked phone is still fixable.

1. Unplug the USB cable from the phone.
2. Press and hold the **BOOT** button on the Luckfox board.
3. While still holding it, plug the USB cable back in.
4. Keep holding for about three more seconds, then let go.

Nothing will appear on the screen. That is correct -- the screen stays dark
in maskrom mode.

### Step 2. Check the computer can see it

```sh
cd /run/media/bubbles/ff277fc3-600e-4010-b73e-76a24c99656c/Projects/luckfox-pico
sudo ./tools/linux/Linux_Upgrade_Tool/upgrade_tool ld
```

You want a line that mentions **Maskrom**, like:

```
DevNo=1  Vid=0x2207,Pid=0x110c,LocationID=102    Maskrom
```

If it says `No found any devices`, Step 1 did not take. Unplug and do Step 1
again -- press the button *before* the cable goes in, not after.

### Step 3. Flash

```sh
cd /run/media/bubbles/ff277fc3-600e-4010-b73e-76a24c99656c/Projects/luckfox-pico
sudo ./rkflash.sh update
```

This writes about 60 MB and takes a few minutes. It prints a percentage.

**Do not unplug the cable while it runs.** If you do, the phone goes back to
being unbootable and you start again at Step 1 -- which is annoying, not
fatal.

It is finished when it prints something like `Upgrade firmware ok`.

### Step 4. Start the phone

Unplug the USB cable, then plug it back in normally (no button this time).

The first boot after a flash is slower than usual: the phone is creating the
User area from scratch. Expect the boot logo for longer than you are used
to. If the screen shows a sad face, the system image failed its integrity
check -- see "If it still will not boot" below.

---

## Part B -- build a new update.img first

Only needed if the `update.img` sitting in the SDK is not the version you
want. Run these from the NeoDCT repo. Each line waits for the one before it.

```sh
cd ~/Documents/Projects/neodct
SDK=/run/media/bubbles/ff277fc3-600e-4010-b73e-76a24c99656c/Projects/luckfox-pico

# 1. Build the operating system. Takes 30-60 minutes the first time,
#    a few minutes if you have built it before.
make -C buildroot O=$PWD/build-luckfox \
    BR2_DEFCONFIG=$PWD/neodct/configs/luckfox_pico_mini_defconfig defconfig
make -C buildroot O=$PWD/build-luckfox

# 2. Build the initramfs -- the tiny system that runs before the real one.
#    Nothing else does this step, and skipping it is how you get a phone
#    that boots a new system with an old installer inside it.
neodct/scripts/mkinitramfs.py \
    --target-dir build-luckfox/target \
    --init neodct/initramfs \
    --verifier build-luckfox/images/nd-verify \
    --recui build-luckfox/images/nd-recui \
    --bootbar build-luckfox/images/nd-bootbar \
    --output build-luckfox/images/initramfs.cpio.gz
gzip -dc build-luckfox/images/initramfs.cpio.gz > build-luckfox/images/initramfs.cpio

# 3. Turn it into the two images the NAND chip wants.
neodct/tools/mknand.sh build-luckfox/images build-luckfox/target build-luckfox/host

# 4. Keep a copy of what is in the SDK now, then hand it the new images.
#    Note the rename: system.ubi becomes rootfs.img. That is not a mistake --
#    "rootfs" is the name of the partition, and the system image goes in it.
D=$(date +%Y%m%d-%H%M)
for f in rootfs.img userdata.img update.img; do
    cp -a "$SDK/output/image/$f" "$SDK/output/image/$f.prev-$D"
done
cp build-luckfox/images/system.ubi   "$SDK/output/image/rootfs.img"
cp build-luckfox/images/userdata.ubi "$SDK/output/image/userdata.img"

# 5. Build the kernel, the boot settings, and finally update.img.
#    This must run inside the luckfox container -- it does not build on Arch.
distrobox enter luckfox -- bash -lc "cd $SDK && ./build.sh kernel && ./build.sh env && ./build.sh updateimg"
```

Then do Part A.

If step 5 dies with `fixdep: error opening file`, the container is tripping
over host tools left behind by an older build. Delete them and run step 5
again:

```sh
rm -rf "$SDK/sysdrv/source/objs_kernel/scripts"
```

---

## If it still will not boot

**Sad face on the screen.** The system image is there but does not match its
signature, so the phone refused to run it. Reflash (Part A). If it happens
twice, the `rootfs.img` in the SDK and the `update.img` disagree -- rebuild
with Part B so both come from the same build.

**Black screen, nothing at all.** The phone is not running. Go back to Step 1
and check the computer can still see it in maskrom mode. If it can, the flash
is recoverable and you just do it again.

**`No found any devices` every time.** Try a different USB cable. A charge-only
cable carries power but no data, and gives exactly this symptom.

## Why this wipes everything and the SD-card update does not

`rkflash.sh update` runs `upgrade_tool uf update.img`, which writes the
partition table and then every partition, `userdata` included. It has no
option to skip one.

The ordinary way to move between versions is an `.ndsw` file on the memory
card, which replaces only the system and leaves User alone. That path needs a
phone that boots. This document is for when that is not true.

See also: `docs/PARTITIONS.md` for the layout being written, and
`docs/HARDWARE_NOTES.md` for flashing single partitions during development.

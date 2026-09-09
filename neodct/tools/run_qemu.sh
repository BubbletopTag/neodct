#!/bin/sh
# Boot the immutable NeoDCT image set under QEMU.
#
# ============ THE EMULATED MACHINE IS THE PHONE'S PART ============
#
# qemu-system-arm -M virt -cpu cortex-a7 -smp 1 -m 64.
#
# It was qemu-system-aarch64 -cpu cortex-a53, and that machine agreed with the
# Pico Mini about everything except the things that break: 64-bit time_t,
# size_t and pointers, and alignment nobody has to think about. Bugs that turn
# on any of those passed here and failed on the bench, which is the one place
# in this project where a failure is expensive to find. One ABI on both
# machines moves them into the emulator.
#
# Measured on this command line, on the kernel this tree builds
# (buildroot/board/qemu/armv7-virt/linux.config):
#
#   uname -m       armv7l, CPU part 0xc07, CPU architecture 7
#   features       neon vfpv3 vfpv4 idiva idivt thumbee
#   MemTotal       54,812 kB of the 64 MB machine; the phone has ~54 MB
#                  (53,824 kB with no -dtb -- see the device-tree block)
#   input_event    16 bytes per record, not the 24 an LP64 host writes
#
# That last line is the shape of the whole change. nd_evdev.c has always
# decoded both record layouts; until now the emulator only ever handed it the
# 24-byte one, so the branch that runs on the phone ran nowhere else.
#
# -smp 1 because the RV1103 has one core. A race that needs two processors to
# show up is a race this phone cannot have, and a test that needs two to find
# it is testing a machine nobody owns.
#
# ============ EVERY VIRTIO DEVICE IS -device, NEVER -pci ============
#
# `-M virt` does have a PCIe host bridge, so QEMU accepts `-device
# virtio-gpu-pci` without a word. The GUEST is what has no PCI: CONFIG_PCI is
# off in the armv7 kernel -- measured, /sys/bus/pci does not exist -- and
# leaving it off is part of how that MemTotal was reached. A -pci device
# therefore attaches to a bus nothing is looking at, and the symptom is a
# phone that boots with a piece of itself missing and nothing in any log.
#
# So: virtio-blk-device, virtio-gpu-device, virtio-keyboard-device,
# virtio-tablet-device, all on virtio-mmio. `qemu-system-arm -M virt -device
# help` lists what exists; `bus virtio-bus` is the column that matters, and
# `bus PCI` means the guest will not see it.
#
# ============ AND -global virtio-mmio.force-legacy=false, OR NO KEYBOARD ====
#
# QEMU's virtio-mmio bus is force-legacy by DEFAULT, so it does not offer
# VIRTIO_F_VERSION_1, and virtinput_probe() opens with
#
#     if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
#             return -ENODEV;
#
# -ENODEV from a probe prints nothing at any loglevel, ignore_loglevel
# included. The device sits in /sys/bus/virtio/devices with an empty driver
# link, /dev/input/event0 never appears, and the phone comes up with no
# keyboard -- which looks like a broken input stack rather than a transport
# option. This never bit on aarch64 because that machine put virtio on PCI,
# and virtio-pci is modern by default. Measured with the flag: driver
# virtio_input, /dev/input/event0, Name="QEMU Virtio Keyboard".
#
# ============ THE DRIVES, AND WHAT THE GUEST CALLS THEM ============
#
# QEMU does not enumerate virtio-mmio devices in the order they are given
# here. It is the reverse, and the letters therefore depend on HOW MANY
# drives are attached. Both measured on this kernel:
#
#   system, user, card   ->   vda=card   vdb=user   vdc=system
#   system, user         ->   vda=user   vdb=system
#
# So no fixed `neodct.sys=` can be right, and this script no longer passes
# one. What identifies a drive is `serial=`: the initramfs reads it back out
# of /sys/block/*/serial (ndsys-apply.sh device_by_serial), which also works
# after "wipe system" has zeroed the image and left nothing on the device to
# recognise it by, and it records what it resolved into verity_state.prop for
# /NeoDCT/System/hw/neodct-sdcard to read.
#
# The hint used to be here and used to say /dev/vda, and it has cost real
# time twice: ndsys-apply.sh:83 and the neodct-sdcard comment that begins
# "neodct.sys=/dev/vda once named the SD card". A wrong hint is worse than
# none, because find_system_device()'s last resort accepts any block device
# it is handed -- on a wiped system that is the card.
#
# ============ WHAT THIS KERNEL CANNOT DO YET ============
#
# The armv7 config is the proven floor for memory parity, not a feature-equal
# replacement for the aarch64 one, and the switches below that ask for
# hardware it has no driver for REFUSE rather than assemble. Every one of them
# would otherwise be accepted by QEMU, ignored by the guest, and leave a phone
# that looks almost right. Measured in the guest:
#
#   /sys/bus/pci           does not exist          CONFIG_PCI off
#   /sys/bus/usb/devices   empty                   every xhci QEMU offers on
#                                                  -M virt is a PCI device and
#                                                  virt has no other USB host
#   /sys/class/net         lo and sit0 only        CONFIG_NETDEVICES off
#
# so audio, the SIM7600 passthrough and the Bluetooth dongle are one missing
# symbol between them, not three. The FAT card and the virtiofs share are two
# more (VFAT_FS, VIRTIO_FS+FUSE_FS). The kernel config's header lists what
# each absence costs and says that adding one means booting it again for a
# fresh MemTotal.
#
# ============ AND THE PANEL, WHICH IS NOW THE PHONE'S OWN DRIVER ============
#
# /dev/fb0 is vfb, the same driver the Luckfox uses, which is the point: the
# emulator stops pretending to be a DRM device and becomes the thing the phone
# is. `video=vfb:on` is what creates it -- the module-parameter form does not
# work, because vfb_init() calls vfb_setup() before testing the flag and
# vfb_setup() opens by zeroing it, so `vfb.vfb_enable=1` produces no
# framebuffer and no line in dmesg.
#
# AND RECOVERY'S TEXT MENU HAS NO SCREEN HERE. This kernel has no
# FRAMEBUFFER_CONSOLE -- it is `default DRM_FBDEV_EMULATION` and the DRM stack
# is deliberately gone -- while CONFIG_VT and DUMMY_CONSOLE keep /dev/tty1 a
# writable character device with nothing behind it. Measured: /proc/consoles
# lists ttyAMA0 alone, /sys/class/graphics/fbcon does not exist. recovery_tty()
# now asks for fbcon rather than trusting the chardev, so the menu comes out on
# /dev/console; NEODCT_RECTTY=/dev/console asks for the same thing explicitly
# and is worth passing so nothing depends on the fallback. nd-recui is
# unaffected -- it draws on /dev/fb0, which is there.
#
# THE MODE IS THE PHONE'S, SET BY THE PHONE'S OWN CODE. vfb comes up at its
# built-in default -- 640x480 at 8 bpp, measured here -- and is put into
# 240x175x32 by a userspace FBIOPUT_VSCREENINFO. On the phone neodct_displayd
# does that; under QEMU S90display now starts the same daemon with
# `--panel null --once`, so the same force_mode() runs on the same driver.
# Measured on a real boot of this kernel: 640,480 / 8 / 640 before, 240,175 /
# 32 / 960 after, red.offset 0 -- the phone's framebuffer byte for byte, and
# force_mode() a tested path for the first time.
#
# AND vfb HAS NO SCANOUT, WHICH IS WHY THE PICTURE IS RENDERED ON THE HOST.
# Nothing a QEMU display frontend shows is the phone and nothing ever will be:
# the pixels are in vfb's memory and no display device reads them. The
# virtio-gpu device below is attached for keystrokes, not pixels -- a frontend
# only routes keys into the guest's virtio keyboard when console 0 is a
# GRAPHIC console. Measured: with no graphics device at all, keys sent over
# VNC went to QEMU's text console and /dev/input/event0 saw none of them; with
# virtio-gpu-device they arrive. With NEODCT_DISPLAY=none there is no frontend
# and the way in is the monitor: NEODCT_MONITOR=/tmp/ndmon, then `sendkey a`
# (also measured).
#
# So the panel image comes out over a virtio-console port instead:
# NEODCT_PANEL_STREAM=<file> attaches one, S90display starts the daemon with
# `--panel stream:/dev/vport0p1`, and every ST7789 command and every pixel the
# panel would have received arrives on the host as an ND79 transcript.
# neodct/tools/st7789_replay.py turns it into a 240x240 PNG -- the composed
# panel, letterbox and all, which is something no frontend here can show.
# Measured: 199,353 bytes for one frame, byte-identical to the same daemon run
# on the host, and MemTotal 54,812 kB with the device attached and 54,812 kB
# without it. The transport is free.
#
# Usage:
#   neodct/tools/run_qemu.sh                  boot normally (writes persist)
#   NEODCT_SNAPSHOT=1 ...                     throw away all writes on exit
#   NEODCT_VERITY=permissive ...              boot even if verity fails
#   NEODCT_VERITY=off ...                     skip verity entirely
#   NEODCT_DEBUG=1 ...                        verbose initramfs, no quiet
#   NEODCT_DEVENV=0 ...                       do NOT source /NeoDCT/User/env.sh
#   NEODCT_UNSIGNED=1 ...                     install unsigned updates
#   NEODCT_APPEND="printk.time=1" ...         extra kernel cmdline (see below)
#   NEODCT_MEM=256 ...                        more RAM than the phone has
#   NEODCT_RTC=epoch ...                      boot with the RTC at 1970-01-01,
#                                             which is the phone's cold boot
#   NEODCT_STORAGE=virtio ...                 /NeoDCT/User back on the ext4
#                                             virtio disk (see the storage
#                                             block below -- it says on every
#                                             boot what it is not testing)
#   NEODCT_STORAGE=nand-full ...              system AND userdata on the
#                                             simulated NAND, through
#                                             /dev/ubiblock0_0. Needs
#                                             NEODCT_MEM>=128 and says why
#   NEODCT_KEYPAD=off ...                     boot with NO i2c keypad, so the
#                                             emulator falls back to the evdev
#                                             path (it says so on every boot)
#   NEODCT_KEYS=<path> ...                    where the keypad fifo lives
#   NEODCT_SD=none ...                        no card attached
#   NEODCT_RECOVERY=1 ...                     boot into recovery mode
#   NEODCT_RECTTY=/dev/console ...            drive recovery over serial
#                                             (recovery's TEXT menu has no
#                                             other way out here -- see below)
#   NEODCT_DISPLAY=none ...                   no panel at all (the UI cannot
#                                             boot; serial/recovery only)
#   NEODCT_DISPLAY=offscreen ...              panel present, no window
#   NEODCT_DISPLAY=vnc ...                    VNC frontend, no desktop needed
#                                             (127.0.0.1:5901; NEODCT_VNC to move it)
#   NEODCT_PANEL_STREAM=/tmp/panel.nd79 ...   write the ST7789 wire stream to
#                                             a host file, then
#                                             st7789_replay.py --out a.png
#   NEODCT_CONSOLE=pipe:/tmp/fifo ...         put the serial console on a
#                                             chardev instead of stdio, for a
#                                             caller that drives the boot
#                                             (parity_capture_qemu.sh)
#   NEODCT_MONITOR=/tmp/ndmon ...             QEMU monitor socket -- sendkey
#
#   NEODCT_SD=share, NEODCT_MODEM, NEODCT_BT, NEODCT_NET and NEODCT_AUDIO
#   refuse on this kernel and say why; see the block above.
#
# Persistence matters for update testing: SystemUpdate stages an update, the
# phone reboots and the initramfs applies it. With NEODCT_SNAPSHOT=1 that
# write is discarded and the update looks like it vanished.
#
# ============ STORAGE: THE PHONE HAS NO BLOCK DEVICES AT ALL ============
#
# The Luckfox has raw NAND behind MTD and nothing else. /NeoDCT/System is a
# squashfs on a static UBI volume published as /dev/ubiblock0_0, and
# /NeoDCT/User is ubifs on `ubi1:userdata`. Every one of those words named a
# code path that had never executed anywhere: user_is_ubi(), the ubifs branch
# of the initramfs's mount, ubi_fit(), ubiupdatevol.
#
# NEODCT_STORAGE=nand is the default and it moves /NeoDCT/User onto the chip.
# nandsim at the Pico Mini's ID bytes is the phone's part exactly -- 128 MB, a
# 128 KiB PEB, a 2048-byte page, 64 bytes of OOB -- `nandsim.parts=` in
# qemu_machine.sh gives it PARTITIONS.md's six partitions at the phone's mtd
# numbers, and a QEMU-only flasher (neodct/initramfs/qemu/ndflash, packed in
# as a second cpio) writes mknand.sh's own userdata.ubi onto mtd4 and attaches
# UBI over it. Measured, at -m 64, on this kernel:
#
#   ubi1: volume 0 ("userdata") re-sized from 13 to 40 LEBs
#   ubi1: PEB size: 131072 bytes, LEB size: 126976 bytes
#   ubi1: VID header offset: 2048 (aligned 2048), data offset: 4096
#   UBIFS (ubi1:0): mounted UBI device 1, volume 0, name "userdata"
#
# ============ AND WHY THE SYSTEM HALF IS NOT ALSO ON IT ============
#
# It was meant to be. It cannot be, at the phone's memory, and both walls were
# measured rather than argued:
#
#   1. WITHOUT a host-backed cache file, nandsim keeps one 2112-byte slab
#      object per WRITTEN page. A 51 MB system.ubi is 26,112 pages = 54,953 kB
#      of unreclaimable slab on a machine with 53,824 kB of usable RAM.
#      Measured at -m 64: the OOM killer takes dd partway through, with
#      `nandsim 40526KB` in the unreclaimable slab report. At -m 128 the same
#      flash completes and the whole stack works -- ubiblock0_0, squashfs,
#      dm-verity over it, ubifs on ubi1 -- which is what NEODCT_STORAGE=nand-full
#      is, and why it refuses below 128 MB rather than pretending.
#
#   2. WITH nandsim.cache_file, the memory cost goes away and the guest
#      DEADLOCKS instead, on the first read of the system volume that misses
#      the cache file's page cache. The stack, caught in the act:
#
#        mount -> squashfs_fill_super -> submit_bio_wait -> __submit_bio
#              -> blk_mq_dispatch_rq_list -> ubiblock_queue_rq -> ubi_leb_read_sg
#              -> mtd_read -> ns_do_state_action -> ns_read_file
#              -> __kernel_read -> blkdev_read_iter -> filemap_read_folio
#
#      nandsim's backing store is a BLOCK DEVICE, so servicing a ubiblock
#      request submits a second bio from inside the first one's dispatch --
#      and /sys/block/vda/stat then stops advancing with 0 in flight, i.e. the
#      nested request never reaches the driver at all. Controlled pair, same
#      cmdline, same image: a mount whose cache-file pages are still resident
#      from the flash succeeds in 0.1 s, and the same mount after
#      `echo 3 > /proc/sys/vm/drop_caches` never returns. So "read the device
#      once to warm it up" is not a fix, it is a coincidence that ends at the
#      first cold region -- and the SRCU stall this looked like at first is
#      blk_mq_timeout_work firing thirty seconds later on the request that is
#      already stuck.
#
#      It does NOT affect ubifs on ubi1, which is why the default mode is
#      possible at all: UBIFS reads UBI directly and stacks no block device on
#      the chip, so nothing nests. Measured across a session boundary.
#
# NEODCT_STORAGE=virtio is the old arrangement, kept, and it prints on every
# boot the list of code paths it is not exercising -- because the reason this
# stage exists is that nobody noticed those paths had never run.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
IMAGES="${NEODCT_IMAGES:-$(dirname "$REPO")/buildroot/output/images}"

# 64 MB is the phone's whole RAM, and this kernel leaves 53,824 kB of it to
# userspace against the phone's ~54 MB -- measured, at this -m, on this
# kernel, and 54,812 kB once the device tree below is passed, which is
# ~1 MB ABOVE the phone rather than below it. The default used to be 72 "to
# stay near the Pico Mini's 64MB", which was a fudge for a kernel fat enough
# that 64 would have been 12 MB HARSHER than the hardware. It is not needed
# any more, and a fudge that says 72 while the phone says 64 is the reason a
# build fits here and not there.
MEMORY="${NEODCT_MEM:-64}"
VERITY="${NEODCT_VERITY:-enforce}"
SD_MODE="${NEODCT_SD:-image}"
DISPLAY_MODE="${NEODCT_DISPLAY:-gtk}"
STORAGE="${NEODCT_STORAGE:-nand}"
KEYPAD="${NEODCT_KEYPAD:-i2c}"
# Set by the device-tree branch below when there is no dtc. The bus NUMBER is
# a device-tree fact, so a tree-less boot cannot have the keypad -- see there.
KEYPAD_NODTB=""

case "$STORAGE" in
    nand|nand-full|virtio) ;;
    *)
        echo "run_qemu: NEODCT_STORAGE must be nand, nand-full or virtio" >&2
        exit 1
        ;;
esac

# The same refusal, for the same reason. It used to accept off|none|0 and
# treat EVERYTHING else as "attach the bus", so NEODCT_KEYPAD=no, =false and
# =OFF all booted WITH the keypad -- and the whole value of the off path is the
# banner naming the paths it is not exercising, so an operator who thought they
# had taken the bus away got the opposite boot and none of the warning.
case "$KEYPAD" in
    i2c|on|1|off|none|0) ;;
    *)
        echo "run_qemu: NEODCT_KEYPAD must be i2c or off" >&2
        exit 1
        ;;
esac

# 128 MB is not a round number picked for comfort: it is the first -m at which
# a 51 MB system.ubi fits, because the chip costs 54,953 kB of unreclaimable
# kernel slab the moment it holds one (see the storage block in the header).
# Refusing is the point -- assembling this mode at -m 64 gets an OOM panic
# eight seconds into the boot, which reads as a broken image.
if [ "$STORAGE" = "nand-full" ] && [ "$MEMORY" -lt 128 ]; then
    echo "run_qemu: NEODCT_STORAGE=nand-full needs NEODCT_MEM=128 or more." >&2
    echo "  A 51 MB system volume on the simulated chip is 26,112 written" >&2
    echo "  pages at 2112 bytes of kernel slab each -- 54,953 kB, measured --" >&2
    echo "  and this guest has ${MEMORY} MB. At -m 64 the OOM killer takes the" >&2
    echo "  flash partway through and panics the guest." >&2
    echo "  THIS MODE IS NOT THE PHONE'S MEMORY. It is the phone's STORAGE," >&2
    echo "  which is the other half of the parity this branch is chasing, and" >&2
    echo "  the two cannot be had at once on this kernel." >&2
    exit 1
fi

# ============ THE PANEL TRANSCRIPT, AND WHY IT IS NOT ALWAYS ON ============
#
# A path here attaches a virtio-console port named neodct.panel and tells the
# guest, on the kernel command line, that something outside /NeoDCT/User has
# asked for a panel stream. S90display then starts neodct_displayd with
# `--panel stream:/dev/vport0p1` instead of `--panel null --once`, and the
# host file fills with the ND79 record stream nd_panel.h pins.
#
# Off by default, for two separate reasons and only one of them is cost:
#
#   * IT CHANGES THE MACHINE. The guest grows /dev/vport0p1 and
#     /sys/class/virtio-ports, both of which nd-inventory records, and
#     dev.count moves. The parity harness must describe the machine somebody
#     boots, so parity_capture_probe.sh deliberately does NOT attach this and
#     runs `--panel null` -- which is why the null backend exists as a
#     configuration rather than being "the stream pointed at /dev/null".
#   * it is a continuous copy of the screen leaving the machine, and the gate
#     for that is the kernel command line rather than anything on the writable
#     partition. Same rule as env.sh, same reason.
#
# It costs no memory: MemTotal 54,812 kB measured with the device and 54,812 kB
# without it, and CONFIG_VIRTIO_CONSOLE=y is already in the kernel config.
PANEL_STREAM="${NEODCT_PANEL_STREAM:-}"
SHARE_DIR="${NEODCT_SHARE:-$HOME/neodct-sdcard}"
MONITOR="${NEODCT_MONITOR:-}"

# Where a vnc display listens. See the `vnc` case below for why the default
# is a loopback address and not a bare ":1".
VNC_ADDR="${NEODCT_VNC:-127.0.0.1:1}"
EXTRA="${NEODCT_QEMU_EXTRA:-}"

# ============ WHERE THE SERIAL CONSOLE GOES, FOR A CALLER THAT DRIVES IT ====
#
# `stdio` by default, which is every interactive session. NEODCT_CONSOLE is
# any QEMU chardev spec -- `pipe:/path/to/fifo`, `file:/path/to/log` -- and it
# REPLACES stdio in every display branch below, because -M virt wires exactly
# one pl011 and a second -serial would not be a second console.
#
# IT EXISTS BECAUSE THERE WAS NO WAY IN AT ALL. parity_capture_qemu.sh ran
# `run_qemu.sh -serial pipe:$FIFO -display none`, and this script parses no
# positional arguments whatsoever: the `set --` that assembles the QEMU
# command line further down OVERWRITES "$@", so both flags were discarded in
# silence. The console then went to stdio as usual, the capture script's fifo
# never received a byte, and it timed out after 180 s with "REFUSED: the guest
# never reached a login prompt" -- which reads as a broken image, on a
# perfectly good one, every time. NEODCT_QEMU_EXTRA could not have rescued it
# either: it is appended AFTER the display branch's own -serial stdio, and the
# first -serial wins.
CONSOLE="${NEODCT_CONSOLE:-stdio}"

# ============ THE FLAGS THIS KERNEL CANNOT HONOUR ============
#
# Each of these names hardware the guest has no driver for. Refusing costs one
# line of reading; assembling it anyway costs a session spent believing the
# feature is broken -- or worse. NEODCT_MODEM=1 is the worst of them: with no
# USB the AT port never enumerates, and on a QEMU build no port at all is
# still simulation, so the phone would place pretend calls to whoever thought
# they were testing a passthrough.
#
# THE WIRING FOR EVERY ONE OF THEM IS LEFT BELOW, INTACT. When the kernel
# carries the symbol again, deleting the matching line here is the whole
# change.
#
# CONFIG_PCI IS NAMED WITH CONFIG_PCI_HOST_GENERIC AND THAT PAIRING MATTERS.
# It said CONFIG_PCI alone, and that is trap 1 in the kernel config's own
# header wearing another hat: measured on this tree's linux.config,
# `./scripts/config -e PCI` then olddefconfig gives CONFIG_PCI=y and, free,
# CONFIG_USB_PCI=y and CONFIG_USB_XHCI_PCI=y -- and leaves
# `# CONFIG_PCI_HOST_GENERIC is not set`. Without the ECAM bridge driver the
# bus on -M virt is never probed, so somebody who set PCI, rebuilt the kernel,
# re-booted for a fresh MemTotal and deleted the refusal would find
# /sys/bus/usb/devices still empty, the AT port still absent -- and, because
# no port at all on a QEMU build is still simulation, the phone placing
# pretend calls at them.
refuse() {   # refuse FLAG SYMBOL WHAT-IT-NEEDED
    echo "run_qemu: $1 cannot work on this kernel." >&2
    echo "  It needs $2, which buildroot/board/qemu/armv7-virt/linux.config" >&2
    echo "  does not carry -- $3" >&2
    echo "  Adding it back means booting the kernel again for a fresh" >&2
    echo "  MemTotal, and then deleting this refusal." >&2
    exit 1
}

[ -n "${NEODCT_NET:-}" ] && refuse NEODCT_NET "CONFIG_NETDEVICES + CONFIG_VIRTIO_NET" \
    "/sys/class/net has lo and sit0 and nothing else."
[ -n "${NEODCT_MODEM:-}" ] && refuse NEODCT_MODEM \
    "CONFIG_PCI + CONFIG_PCI_HOST_GENERIC (for an xhci)" \
    "every USB host controller -M virt offers is a PCI device."
[ -n "${NEODCT_BT:-}" ] && refuse NEODCT_BT \
    "CONFIG_PCI + CONFIG_PCI_HOST_GENERIC (for an xhci) and CONFIG_BT" \
    "neither the dongle's bus nor the stack above it is built."
[ "$SD_MODE" = "share" ] && refuse "NEODCT_SD=share" "CONFIG_VIRTIO_FS + CONFIG_FUSE_FS" \
    "vhost-user-fs attaches and nothing in the guest can mount it."

# Audio was probed rather than assumed, because `-audiodev pa` on a machine
# with no PulseAudio daemon makes QEMU refuse to start AT ALL -- two fatal
# errors before the kernel loads ("XDG_RUNTIME_DIR not set", "could not stat
# pidfile /run/xdg/pulse/pid"), neither of which says the word audio. That
# probe is worth restoring with the rest of the USB wiring; today there is no
# bus to put a usb-audio device on, so the answer is none whatever the host
# has running. An explicit request still gets an answer rather than silence.
if [ -n "${NEODCT_AUDIO:-}" ] && [ "$NEODCT_AUDIO" != "none" ]; then
    refuse "NEODCT_AUDIO=$NEODCT_AUDIO" \
        "CONFIG_PCI + CONFIG_PCI_HOST_GENERIC (for an xhci)" \
        "usb-audio has nothing to hang off: /sys/bus/usb/devices is empty."
fi
AUDIO=none

# SIM7600 as seen on the USB bus.
MODEM_VENDOR="${NEODCT_MODEM_VENDOR:-0x1e0e}"
MODEM_PRODUCT="${NEODCT_MODEM_PRODUCT:-0x9001}"

# TP-Link UB500, an RTL8761BU. Overridable because any btusb-class dongle
# works: btusb matches on the USB class (e0/01/01), not on the id.
BT_VENDOR="${NEODCT_BT_VENDOR:-0x2357}"
BT_PRODUCT="${NEODCT_BT_PRODUCT:-0x0604}"

# zImage, not Image: an arm kernel builds a self-decompressing zImage and
# Buildroot names it that (BR2_LINUX_KERNEL_ZIMAGE). A tree still holding an
# aarch64 `Image` is a tree whose .config predates the defconfig -- which is
# the failure AGENTS.md opens with -- so the missing file is the right thing
# to complain about.
REQUIRED="zImage initramfs.cpio.gz system.img"
[ "$STORAGE" = "virtio" ] && REQUIRED="$REQUIRED userdata.ext4"
[ "$STORAGE" = "nand-full" ] && REQUIRED="$REQUIRED system.ubi"
for required in $REQUIRED; do
    if [ ! -f "$IMAGES/$required" ]; then
        echo "run_qemu: $IMAGES/$required missing." >&2
        echo "  Build with: cd buildroot && make neodct_qemu_defconfig && make" >&2
        exit 1
    fi
done

# userdata.ubi gets its own refusal because the cause is almost always "this
# image predates the NAND storage mode" rather than "the build failed", and
# the two need different answers. post-image-neodct.sh builds it from the same
# skeleton as userdata.ext4 -- it has to, because that skeleton carries
# .ndsys/installed.prop and dm-verity has no root hash without it.
if [ "$STORAGE" != "virtio" ] && [ ! -f "$IMAGES/userdata.ubi" ]; then
    echo "run_qemu: $IMAGES/userdata.ubi missing, so /NeoDCT/User cannot go on" >&2
    echo "  the NAND. Either the image was built before this storage mode" >&2
    echo "  existed -- rebuild, post-image-neodct.sh writes it beside" >&2
    echo "  userdata.ext4 -- or mtd-utils was not available to that build and" >&2
    echo "  post-image said so. NEODCT_STORAGE=virtio boots the old way." >&2
    exit 1
fi

# ============ THE DEVICE TREE, BUILT FRESH ON EVERY RUN ============
#
# `-M virt` generates its own device tree and hands it to the kernel. Two
# things the phone has are not in it, and both are drivers this kernel has
# been carrying with nothing to bind to since the armv7 config landed:
#
#   cpufreq   cpu@0 declares neither `clocks` nor `operating-points-v2`, so
#             cpufreq_dt_platdev_init() creates no platform device and
#             /sys/devices/system/cpu/cpu0/cpufreq -- ND_CPUFREQ_DIR, the only
#             path nd_cpufreq.c opens -- does not exist. Measured.
#   backlight -M virt has no PWM controller of any kind, so pwm-backlight
#             never probes and /sys/class/backlight is empty. Measured.
#
# neodct/board/qemu/nd-virt-additions.dtsi supplies both, plus a software PWM
# to hang the backlight off. It is APPENDED to QEMU's own decompiled tree and
# the whole thing recompiled -- not applied as an overlay, which does not work
# (`dtc -@` refuses a value reference to a base-tree node by path, and QEMU's
# blob carries no __symbols__ for a label form to resolve against).
#
# NOTHING IS COMMITTED AND NOTHING IS CACHED ACROSS QEMU VERSIONS. `-M virt`
# is a versioned machine: a blob cut today still boots on next year's QEMU --
# only /memory and /chosen get patched -- so a committed one would freeze the
# guest's device set to whatever QEMU produced the day it was cut, silently.
# Regenerating from the installed binary every run cannot drift. Measured, so
# that the dump can be this cheap: a tree dumped with only -M/-cpu/-smp/-m is
# byte-identical to one dumped with the whole device set attached, apart from
# rng-seed and kaslr-seed.
#
# AND NEODCT_MEM STILL WORKS. QEMU rewrites /memory in a user-supplied DTB --
# measured: a tree dumped at -m 64, booted at -m 256, gives MemTotal
# 249,740 kB -- so the tree does not have to be regenerated per memory size.
#
# THE ONE PRICE IS ~1 MB OF GUEST MEMORY. The kernel reserves
# fdt_totalsize(), QEMU's own blob is padded to 1 MiB and a dtc-produced one
# is 8 KB, so handing over a smaller tree gives the reservation back: MemTotal
# 53,824 kB with no -dtb, 54,812 kB with one. The emulator moves from ~180 kB
# below the phone's ~54 MB to ~800 kB above it. That is written down in the
# kernel config's header rather than papered over with a pad size -- measured,
# a 1 MiB dtc pad does not boot at all.
NDDTB_DIR="${TMPDIR:-/tmp}/neodct-qemu-dtb"
NDDTB="$NDDTB_DIR/nd.dtb"
DTSI="$REPO/board/qemu/nd-virt-additions.dtsi"
# The recipe itself is in qemu_machine.sh, shared with test_qemu_surfaces.sh
# and parity_capture_probe.sh so that neither can go on asserting a backlight
# against a recipe this script no longer uses. That file now holds the KERNEL
# PARAMETERS too -- see nd_qemu_append() and the block below.
#
# Sourcing it is no longer optional and its absence is no longer silent: the
# device tree half degrades (a session with no backlight is still worth
# having, see below) but the parameter half does not -- without nandsim's ID
# bytes the guest comes up on a 16 KiB erase block that is not the phone's,
# and without mtdram.total_size=0 it vmallocs 4 MiB out of a 64 MB machine.
if [ -r "$HERE/qemu_machine.sh" ]; then
    . "$HERE/qemu_machine.sh"
else
    echo "run_qemu: $HERE/qemu_machine.sh is missing. It carries the device tree" >&2
    echo "  AND the kernel parameters that decide which devices this guest has, so" >&2
    echo "  a session assembled without it is not the machine anything else here" >&2
    echo "  measures. Refusing rather than booting a different emulator." >&2
    exit 2
fi

# Loud rather than fatal. Without a device tree the emulator is still the phone
# in every other respect, and refusing to boot over a backlight would be the
# worse trade -- but it has to be SAID, or the next person spends an afternoon
# on nd_cpufreq_read_table() returning ND_ERR_NOTFOUND.
if ! command -v nd_dtb_build >/dev/null 2>&1 \
   || ! nd_dtb_build "$DTSI" "$NDDTB_DIR" "$NDDTB"; then
    echo "run_qemu: booting with QEMU's own device tree." >&2
    echo "  That means NO backlight (/sys/class/backlight stays empty) and NO" >&2
    echo "  cpufreq (ND_CPUFREQ_DIR does not exist), so Sleepy's two screens" >&2
    echo "  will both report that there is nothing there." >&2
    # AND THE THIRD ONE, WHICH IS NOT A DEGRADED DEVICE BUT A WRONG ONE.
    #
    # The keypad's BUS NUMBER is a device-tree fact: virtio_mmio.c never sets
    # an of_node, so the only thing that puts the adapter at three is
    # nd-virt-additions.dtsi's disabled reservation node reserving 0..2.
    # Measured, same kernel, same nd_qemu_i2c_args, -dtb omitted:
    # /sys/class/i2c-dev is [i2c-0] and the node is /dev/i2c-0.
    #
    # A bus at the wrong number is WORSE than no bus. nd_pcf8575_open(bus=3)
    # gets ENOENT, nd_input falls back to evdev, nd_battery goes back to SIM,
    # and T9, the first-boot wizard and KeypadMapperI2C are all silently off --
    # while this script would have printed "i2c keypad on /dev/i2c-3" and the
    # fifo would swallow keystrokes without a word. That is precisely the
    # silent fast path the keypad block below exists to refuse, so the bus
    # goes away with the tree rather than coming up somewhere else.
    echo "  AND NO KEYPAD BUS: the adapter's NUMBER comes from the tree's" >&2
    echo "  reservation node, so without it the bus is /dev/i2c-0 and nothing" >&2
    echo "  looks for it there. Booting on evdev, with no matrix, no T9, no" >&2
    echo "  first-boot wizard and a simulated battery. Install dtc." >&2
    NDDTB=""
    KEYPAD_NODTB=1
fi

# ============ THE NAND, AND THE FACTORY THAT WRITES IT ============
#
# Everything in this block is skipped entirely by NEODCT_STORAGE=virtio.
#
# NAND_WORK holds the three things a NAND boot needs and none of them is
# committed or cached across runs, for the same reason the device tree is not:
# an artefact that cannot drift is worth more than one that can.
# $$ throughout, so two sessions on one machine cannot hand each other a
# half-written flasher archive or save each other's userdata partition -- the
# same rule nd_dtb_build() already follows for the device tree.
NAND_WORK="${TMPDIR:-/tmp}/neodct-qemu-nand.$$"
NAND_INITRD="$IMAGES/initramfs.cpio.gz"
NAND_CACHE=""
NAND_USERDATA=""
# Where a session's /NeoDCT/User survives to the next QEMU process. It sits
# beside userdata.ext4 and means the same thing: the writable partition, kept.
NAND_SAVED="$IMAGES/userdata.nand.img"

# ============ ONE EXIT PATH, BECAUSE THERE ARE NOW THINGS TO DO ON IT ======
#
# This script used to end in `exec`, and everything below could be a leak or a
# missing save without anybody noticing, because there was no "below". There
# is now -- a NAND session has to lift /NeoDCT/User out of the cache file --
# and the first draft of that got all three of the ways a session ends wrong:
#
#   * `qemu-system-arm ...` followed by `QEMU_STATUS=$?` under `set -eu` is
#     DEAD CODE on the failure path. errexit terminates the script AT the
#     qemu line the moment it returns non-zero, so the save, both of its
#     messages and the cleanup were unreachable on every abnormal exit. Not a
#     corner: `NEODCT_DISPLAY=gtk` with no DISPLAY is `gtk initialization
#     failed`, exit 1, and the session's writes gone with nothing said.
#     Reproduced: `set -eu; false; S=$?; echo reached` prints nothing.
#   * a SIGTERM while the shell waits on a FOREGROUND child does not reach
#     that child. Measured: kill the wrapper and `sleep` is reparented to PID
#     1 and runs on. neodct/tools/test_update_e2e.sh and
#     test_remoteshell_e2e.sh both hold run_qemu.sh's pid and `kill` it to end
#     a boot, and that worked only for as long as this script exec'd. So QEMU
#     is started in the BACKGROUND and waited for, which is the only shape in
#     which a trap can run while it is still alive.
#   * `&` without an explicit redirection assigns the child /dev/null for
#     stdin (POSIX), and stdin is the serial console -- the two e2e harnesses
#     feed it from a fifo. `<&0` does not rescue it: measured, dash applies
#     the /dev/null assignment first, so fd 0 is already gone by the time the
#     redirection is read (bash does not, which is exactly the sort of
#     difference that ships). Hence `exec 3<&0` here and `<&3` there: fd 3 is
#     saved while fd 0 is still the terminal.
#
# The INT and TERM traps exit rather than doing the work, so the EXIT trap is
# the single place a session is wound up and it cannot run twice.
exec 3<&0
QEMU_PID=""
QEMU_RAN=""
VIRTIOFSD_PID=""
KEYPADD_PID=""
KEYPADD_WATCH_PID=""

nd_nand_save() {
    # The de-interleave is in mkqemuflash.py, with the argument for it and for
    # the one rule that is not obvious -- an all-zero page comes back as 0xFF,
    # because a page nandsim never programmed reads as zeros out of a sparse
    # host file and zeros in a UBI partition are a corrupted erase counter
    # rather than free space. Proven by round trip: a session's files came
    # back byte-identical through a second QEMU process with 0 corrupted PEBs.
    #
    # Only for the mode that has a cache file to lift out of, and only once
    # QEMU has actually run: an `exit 1` from the assembly above has nothing
    # to save, and saying "nothing was written" there would be an answer to a
    # question nobody asked.
    [ -n "$QEMU_RAN" ] || return 0
    [ -n "$NAND_CACHE" ] || return 0
    # -snapshot makes the cache drive copy-on-write too, so the host file is
    # untouched and the lift would report "nothing was written" -- true, and
    # an answer to a question nobody asked. The banner above already said it.
    [ -z "${NEODCT_SNAPSHOT:-}" ] || return 0
    if "$REPO/tools/mkqemuflash.py" save \
            --cache "$NAND_CACHE" --out "$NAND_SAVED" > /dev/null 2>&1; then
        echo "run_qemu: /NeoDCT/User saved to $NAND_SAVED" >&2
    else
        echo "run_qemu: nothing was written to the userdata partition this" >&2
        echo "  session, so $NAND_SAVED is left as it was." >&2
    fi
    return 0
}

nd_session_end() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
        QEMU_PID=""
    fi
    if [ -n "$VIRTIOFSD_PID" ]; then
        kill "$VIRTIOFSD_PID" 2>/dev/null || true
        VIRTIOFSD_PID=""
    fi
    # The watchdog goes FIRST, before the daemon it is watching. It exists to
    # notice the daemon dying under a running guest; killing the daemon on the
    # way out is not that, and a watchdog still alive at that moment would
    # print the alarm on every clean shutdown.
    if [ -n "$KEYPADD_WATCH_PID" ]; then
        kill "$KEYPADD_WATCH_PID" 2>/dev/null || true
        KEYPADD_WATCH_PID=""
    fi
    # The keypad daemon, on the EXIT trap and not after the QEMU line -- the
    # same discipline and the same measured reason as the userdata lift: under
    # `set -e` a non-zero QEMU exit terminates this script before anything
    # after that line, and a `kill` of the script skips it too. A daemon left
    # behind holds the socket, and the NEXT boot's QEMU then connects to a
    # backend serving a keypad nobody is typing on.
    if [ -n "$KEYPADD_PID" ]; then
        kill "$KEYPADD_PID" 2>/dev/null || true
        KEYPADD_PID=""
    fi
    nd_nand_save
    rm -rf "$NAND_WORK"
    return 0
}

trap 'nd_session_end' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [ "$STORAGE" != "virtio" ]; then
    rm -rf "$NAND_WORK"
    mkdir -p "$NAND_WORK"

    # --- nd-ubiattach, cross-compiled here rather than shipped ------------
    #
    # It is QEMU-only (its own header says why it must not be in the image),
    # so there is nowhere in the rootfs to take it from. Buildroot's own
    # toolchain first, because a tree that built this image has one; a system
    # cross compiler second. Static, so it needs nothing from the initramfs.
    UBIATTACH="${NEODCT_UBIATTACH:-}"
    if [ -z "$UBIATTACH" ]; then
        NDCC=""
        # $CROSS_COMPILE is only consulted when it is SET. `${CROSS_COMPILE:-}gcc`
        # with it unset is the host's own gcc, which happily produces an x86-64
        # binary that the archive carries into the guest -- where exec fails with
        # ENOEXEC, the shell falls back to interpreting it, and the first line of
        # the boot log is `/bin/nd-ubiattach: line 1: ELF: not found`. Found by
        # booting it.
        for candidate in "$IMAGES"/../host/bin/arm-*-linux-*-gcc \
                         ${CROSS_COMPILE:+"${CROSS_COMPILE}gcc"} \
                         arm-linux-gnueabihf-gcc arm-linux-gnueabi-gcc; do
            command -v "$candidate" > /dev/null 2>&1 && { NDCC="$candidate"; break; }
        done
        if [ -z "$NDCC" ]; then
            echo "run_qemu: NEODCT_STORAGE=$STORAGE needs an armv7 cross compiler to" >&2
            echo "  build nd-ubiattach, and neither buildroot's" >&2
            echo "  ($IMAGES/../host/bin/arm-*-gcc) nor arm-linux-gnueabihf-gcc is" >&2
            echo "  on \$PATH. NEODCT_UBIATTACH=<a static armv7 binary> skips this;" >&2
            echo "  NEODCT_STORAGE=virtio skips the NAND altogether." >&2
            exit 1
        fi
        UBIATTACH="$NAND_WORK/nd-ubiattach"
        "$NDCC" -static -O2 -o "$UBIATTACH" "$REPO/src/tools/nd_ubiattach.c" \
            || { echo "run_qemu: $NDCC could not build nd_ubiattach.c" >&2; exit 1; }
    fi

    # --- the flasher overlay ---------------------------------------------
    #
    # The kernel accepts CONCATENATED cpio archives and unpacks them in order
    # into one rootfs, so the QEMU-only half is a second archive rather than
    # anything inside neodct/initramfs/. rdinit=/ndflash then runs the factory
    # first and it exec's the phone's own /init, which is PID 1 with argv0
    # /init exactly as the kernel would have started it. Measured.
    "$REPO/tools/mkqemuflash.py" overlay \
        --out "$NAND_WORK/ndflash.cpio.gz" \
        --flasher "$REPO/initramfs/qemu/ndflash" \
        --ubiattach "$UBIATTACH" > /dev/null \
        || { echo "run_qemu: could not build the flasher overlay" >&2; exit 1; }
    cat "$IMAGES/initramfs.cpio.gz" "$NAND_WORK/ndflash.cpio.gz" \
        > "$NAND_WORK/initramfs+ndflash.cpio.gz"
    NAND_INITRD="$NAND_WORK/initramfs+ndflash.cpio.gz"

    # --- which userdata image the factory writes --------------------------
    #
    # The saved one, unless the build has moved on underneath it. installed.prop
    # inside that partition records the root hash of the system image that was
    # installed; a rebuild changes the image, and dm-verity then refuses to
    # boot the new system against the old hash. The ext4 path solves that with
    # debugfs; here the honest answer is to start again and say so.
    NAND_USERDATA="$IMAGES/userdata.ubi"
    if [ -f "$NAND_SAVED" ]; then
        if [ "$NAND_SAVED" -nt "$IMAGES/userdata.ubi" ]; then
            NAND_USERDATA="$NAND_SAVED"
        else
            echo "run_qemu: $NAND_SAVED is older than the build; starting" >&2
            echo "  /NeoDCT/User again from userdata.ubi, because its" >&2
            echo "  installed.prop names a system image that no longer exists." >&2
            rm -f "$NAND_SAVED"
        fi
    fi

    # --- the chip's backing store -----------------------------------------
    #
    # 65,536 pages of 2048+64. Sparse and recreated every boot: nandsim's
    # pages_written bitmap is per-boot, so a kept cache file reads as blank
    # silicon however good its bytes are -- a 57 MB artefact that would look
    # like state and behave like nothing. What DOES carry over is
    # userdata.nand.img, lifted back out of this file when QEMU exits.
    #
    # `nand` only. nand-full deliberately has no cache file (a ubiblock read
    # through one deadlocks the guest -- see the header), so creating one there
    # would leave a 132 MB file nothing ever opens.
    if [ "$STORAGE" = "nand" ]; then
        NAND_CACHE="$NAND_WORK/nandcache.raw"
        : > "$NAND_CACHE"
        # 138,412,032 = 65536 * 2112. dd rather than truncate so that a host
        # without GNU coreutils still gets a sparse file of the right length.
        dd if=/dev/null of="$NAND_CACHE" bs=1 seek=138412032 2>/dev/null \
            || { echo "run_qemu: could not create $NAND_CACHE" >&2; exit 1; }
    fi
fi

set -- \
    -M virt \
    -cpu cortex-a7 \
    -smp 1 \
    -m "$MEMORY" \
    -global virtio-mmio.force-legacy=false \
    -kernel "$IMAGES/zImage" \
    -initrd "$NAND_INITRD"

# ============ THE DRIVES ============
#
# NDSYS is the system image and it keeps its serial in every mode, because
# find_system_device()'s first rule is device_by_serial("NDSYS") and it
# outranks everything else. The FLASHING sources deliberately do NOT use it:
# a raw UBI image labelled NDSYS would be picked as the system partition, and
# NDUSER likewise. They are NDNANDSYS and NDNANDUSR, they are readonly=on, and
# both begin "UBI#" rather than "hsqs" so the squashfs scan cannot mistake
# them either.
case "$STORAGE" in
    virtio)
        set -- "$@" \
            -drive "file=$IMAGES/system.img,if=none,format=raw,id=ndsys" \
            -device virtio-blk-device,drive=ndsys,serial=NDSYS \
            -drive "file=$IMAGES/userdata.ext4,if=none,format=raw,id=nduser" \
            -device virtio-blk-device,drive=nduser,serial=NDUSER
        ;;
    nand)
        # The system stays on virtio-blk here and that is a MEASURED limit,
        # not an oversight -- the header's storage block has both numbers.
        set -- "$@" \
            -drive "file=$IMAGES/system.img,if=none,format=raw,id=ndsys" \
            -device virtio-blk-device,drive=ndsys,serial=NDSYS \
            -drive "file=$NAND_USERDATA,if=none,format=raw,readonly=on,id=ndnandusr" \
            -device virtio-blk-device,drive=ndnandusr,serial=NDNANDUSR
        ;;
    nand-full)
        set -- "$@" \
            -drive "file=$IMAGES/system.ubi,if=none,format=raw,readonly=on,id=ndnandsys" \
            -device virtio-blk-device,drive=ndnandsys,serial=NDNANDSYS \
            -drive "file=$NAND_USERDATA,if=none,format=raw,readonly=on,id=ndnandusr" \
            -device virtio-blk-device,drive=ndnandusr,serial=NDNANDUSR
        ;;
esac

[ -n "$NDDTB" ] && set -- "$@" -dtb "$NDDTB"

# ============ THE HARDWARE CLOCK, AND THE ONE CONDITION IT NEVER HAD ========
#
# QEMU's PL031 comes up at the HOST's wall clock, so the phone's cold boot --
# the first sentence of nd_clock.c, "RTC boots at the Unix epoch, and every
# TLS certificate on the internet is not valid yet" -- has never once been
# reproducible under emulation, and neither has nd_clock_apply_floor(), which
# exists for nothing else. One existing QEMU flag is that condition exactly.
# Measured: "rtc-pl031 9010000.pl031: setting system clock to
# 1970-01-01T00:00:02 UTC", /sys/class/rtc/rtc0/date 1970-01-01.
#
# The default is deliberately unchanged: a phone whose clock is wrong on every
# ordinary boot would make every ordinary boot about the clock.
case "${NEODCT_RTC:-host}" in
    host)  ;;
    epoch) set -- "$@" -rtc base=1970-01-01 ;;
    *)
        echo "run_qemu: NEODCT_RTC must be host or epoch" >&2
        exit 1
        ;;
esac

# --- the removable card ---------------------------------------------------
case "$SD_MODE" in
    image)
        if [ -f "$IMAGES/sdcard.img" ]; then
            set -- "$@" \
                -drive "file=$IMAGES/sdcard.img,if=none,format=raw,id=ndsd" \
                -device virtio-blk-device,drive=ndsd,serial=NDCARD
            # No warning here, and the absence is deliberate: an earlier
            # draft of this branch printed "this kernel has no VFAT_FS;
            # nothing in the guest can mount it" on every boot, which was
            # wrong twice over. A NeoDCT card has been ONE EXT4 PARTITION
            # since 0.5.0b -- sdcard.sh:170 and post-image-neodct.sh:10 say
            # so, and FAT was dropped precisely because it cannot record who
            # owns a file -- and EXT4_FS is in this kernel. Verified by
            # mounting a real one in the guest: "EXT4-fs (vda): mounted
            # filesystem ... r/w with ordered data mode".
            #
            # A warning that tells the owner a working card is unusable costs
            # more than no warning at all: it is believed, and the next person
            # goes looking for a kernel symbol instead of at their card.
        else
            echo "run_qemu: no sdcard.img; booting with no card" >&2
        fi
        ;;
    share) ;;   # refused above; the virtiofs wiring is still at the bottom
    none)  ;;
    *)
        echo "run_qemu: NEODCT_SD must be image, share or none" >&2
        exit 1
        ;;
esac

# ============ THE CACHE DRIVE, AND WHY IT IS ATTACHED HERE ============
#
# nandsim opens `nandsim.cache_file=` at device_initcall, before devtmpfs
# exists, so it can only be given a literal path -- and QEMU enumerates
# virtio-mmio in REVERSE, so the LAST virtio-blk-device on the command line is
# the one the guest calls /dev/vda. Measured three times, on this kernel.
#
# THIS BLOCK MUST THEREFORE STAY AFTER THE CARD, AND ANY NEW virtio-blk DEVICE
# MUST GO ABOVE IT. That is exactly the class of positional rule this
# repository has been bitten by twice (ndsys-apply.sh:83, the neodct-sdcard
# comment), so it is defused rather than trusted: ndflash's first act is to
# refuse unless /dev/vda's serial is NDNAND, and nandsim writes nothing to the
# cache device until a page is programmed -- measured, a boot that flashed
# nothing left the host file at 0 blocks allocated -- so that refusal lands
# before anything could be damaged.
#
# A misorder that made one of the readonly=on image drives vda does NOT stop
# nandsim at init, which is what this comment used to say. Measured: the open
# succeeds on a read-only block device and the chip appears; the failure is
# `[nandsim] error: prog_page: write error` at the first program, so the
# flasher's dd fails and it refuses there. Loud, one step later. ndflash's
# header has both this and the worse case -- a cache_file path that does not
# exist, where O_CREAT makes a regular file in the initramfs and puts the
# whole chip in guest RAM with nothing saying so.
#
# nand-full deliberately gets NO cache file: with one, the first read of the
# system volume that misses the cache file's page cache deadlocks the guest,
# because servicing a ubiblock request then submits a second bio from inside
# the first one's dispatch. The header's storage block has the stack trace and
# the controlled pair. The price is that nand-full keeps the whole chip in
# kernel slab -- hence its 128 MB floor -- and that /NeoDCT/User does not
# survive that mode's exit, because there is no host-side file to lift it out
# of.
if [ "$STORAGE" = "nand" ]; then
    set -- "$@" \
        -drive "file=$NAND_CACHE,if=none,format=raw,id=ndnandcache" \
        -device virtio-blk-device,drive=ndnandcache,serial=NDNAND
fi

# --- kernel cmdline ------------------------------------------------------
# One console. `console=ttyS0,115200` was here for a machine that had an
# 8250; -M virt has a PL011 and nothing else (/proc/consoles lists ttyAMA0
# alone), and the image's only getty is on ttyAMA0. A console= naming a port
# that does not exist is silently dropped, which is exactly the sort of line
# that outlives the machine it was written for.
#
# vt.global_cursor_default=0 keeps the VT cursor off the UI, and quiet /
# loglevel=0 keep kernel messages from drawing over it.
#
# ============ AND THE TWO MTD SIMULATORS, WHICH ARE NOT FREE ============
#
# CONFIG_MTD_MTDRAM and CONFIG_MTD_NAND_NANDSIM are both built in, and both
# instantiate themselves with no parameters at all. Measured at -m 64 on this
# kernel, on this exact cmdline:
#
#   without these two arguments   mtd0 mtdram 4 MiB (vmalloc'd out of the
#                                 guest) + mtd1 nandsim at ITS default 16 KiB
#                                 PEB / 512-byte page, MemFree 37,220 kB
#   with them                     one device, erase 131072, write 2048 --
#                                 the Pico Mini's part -- MemFree 42,120 kB
#
# So ~4.8 MB of a 64 MB machine on every ordinary boot, and MemTotal is
# 53,824 kB either way, which is why the number this whole file quotes could
# never show it. The second half matters as much: anybody who runs ubiattach
# in a plain session was getting nandsim's 16 KiB/512 B geometry, i.e. exactly
# the wrong LEB arithmetic test_update_ubi.sh was rewritten to escape, with
# nothing in the guest saying the chip is not the phone's.
#
# The ID bytes are the Pico Mini's, decoded in test_update_ubi.sh's header:
# 0x20/0xa1 is a 1 Gbit x8 part and 0x15 is 2 KiB page, 128 KiB erase block.
# NEODCT_APPEND is appended after this, so a session that wants a different
# chip -- or mtdram back -- still says so and wins.
#
# ============ AND THE DEVICE PARAMETERS ARE NOT WRITTEN HERE ============
#
# nandsim, mtdram and the mock GPIO chip -- which is where gpio53, 56 and 57
# come from, i.e. nd_backlight.c's GPIO tier and the panel's RST and DC --
# live in nd_qemu_append() in qemu_machine.sh, beside nd_dtb_build(), and are
# shared with test_qemu_surfaces.sh and parity_capture_probe.sh. They were
# written out here and hand-copied into both of those, which is how the three
# copies came to differ while the surfaces test's comment still called them
# "run_qemu.sh's parameters": deleting one from this line left that test
# asserting the old machine against its own private command line and passing.
# The argument for every parameter in the set is in that function.
APPEND="console=ttyAMA0 neodct.verity=$VERITY $(nd_qemu_append)"

# ============ THE STORAGE HALF OF THE COMMAND LINE ============
#
# Three of these four keys are carried VERBATIM from docs/PARTITIONS.md
# section 6, which is the phone's own U-Boot environment:
#
#     ubi.block=0,system  neodct.sys=/dev/ubiblock0_0  neodct.user=ubi1:userdata
#
# The fourth, `ubi.mtd=`, cannot be here and that asymmetry is recorded in
# allow.txt rather than hidden: it is consumed by ubi_init_attach(), a
# late_initcall, which runs before any userspace flasher can exist. On a blank
# chip UBI does the worst possible thing with it -- it succeeds and FORMATS
# the partition, after which a raw image cannot be written over it at all,
# because programming NAND only clears bits. ndflash does the attach instead,
# through the same ioctl, at the phone's VID header offset.
#
# `ubi.block=` DOES survive, and that is the good part: ubiblock_notify() on
# UBI_VOLUME_ADDED calls ubiblock_create_from_param(), so a volume attached
# from userspace later still matches the cmdline. Measured -- the emulator's
# /dev/ubiblock0_0 is created by the KERNEL, from the phone's own argument.
case "$STORAGE" in
    nand)
        APPEND="$APPEND neodct.user=ubi1:userdata nandsim.cache_file=/dev/vda"
        ;;
    nand-full)
        APPEND="$APPEND neodct.user=ubi1:userdata"
        APPEND="$APPEND ubi.block=0,system neodct.sys=/dev/ubiblock0_0"
        ;;
esac
[ "$STORAGE" = "virtio" ] || APPEND="$APPEND rdinit=/ndflash"
# Boot straight into recovery. NEODCT_RECTTY=/dev/console drives it over the
# serial port instead of the emulated screen.
[ -n "${NEODCT_RECOVERY:-}" ] && APPEND="$APPEND neodct.recovery=1"
[ -n "${NEODCT_RECTTY:-}" ] && APPEND="$APPEND neodct.rectty=$NEODCT_RECTTY"
if [ -n "${NEODCT_DEBUG:-}" ]; then
    APPEND="$APPEND neodct.debug=1"
else
    APPEND="$APPEND quiet loglevel=0"
fi
# /NeoDCT/User/env.sh is sourced as root before the UI starts, which is why
# run_neodct.sh now refuses to do it unless something OUTSIDE the writable
# partition says to -- SECURITY-AUDIT.md section 4 Q5 vector 2. In QEMU that
# something is this line, and it is on by default because the whole point of
# the emulator is to flip switches without rebuilding an image.
#
# NEODCT_DEVENV=0 takes it away, which is how to see what a shipped phone
# does with an env.sh somebody left on the partition.
[ "${NEODCT_DEVENV:-1}" = "0" ] || APPEND="$APPEND neodct.devenv=1"

# The initramfs now refuses to install a staged update whose manifest is not
# signed by the release key -- SECURITY-AUDIT.md section 3, the critical
# finding. Engineering mode can still build and stage an UNSIGNED package on
# purpose, and testing that path end to end needs the boot side to allow it.
#
# Off by default here as well as on the phone: an unsigned update installing
# silently in QEMU is how a signature check stops being tested.
[ -n "${NEODCT_UNSIGNED:-}" ] && APPEND="$APPEND neodct.unsigned=1"

# Anything else you want on the kernel command line, appended after everything
# this script decides and ahead of only the display case's video=, so it wins
# over the rest. The reason it exists: printk.time=1. CONFIG_PRINTK_TIME is
# off in both kernel configs, so a boot log has no timestamps at all and
# "which part of the boot is slow" cannot be answered without rebuilding the
# kernel. test_update_ubi.sh is the other caller -- it is how nandsim is given
# the Pico Mini's chip ID.
#
#     NEODCT_APPEND="printk.time=1 initcall_debug" NEODCT_DEBUG=1 run_qemu.sh
[ -n "${NEODCT_APPEND:-}" ] && APPEND="$APPEND $NEODCT_APPEND"

# --- display -------------------------------------------------------------
# `video=vfb:on` is what creates /dev/fb0, and the option string has to be
# non-empty -- see the panel note in the header. The mode is NOT set here:
# vfb comes up 640x480x8 on both machines and the UI's force_mode() puts it
# into 240x175x32, which is the arrangement the phone has always had and the
# reason the old `video=Virtual-1:240x175M` is gone. That named a DRM
# connector on the virtio-gpu, and this kernel has no DRM at all.
case "$DISPLAY_MODE" in
    none)
        # -nographic is -display none plus -serial stdio in one flag, so a
        # caller that named a console has to be given the pieces instead.
        if [ "$CONSOLE" = "stdio" ]; then
            set -- "$@" -nographic
        else
            set -- "$@" -display none -serial "$CONSOLE" -monitor none
        fi
        ;;
    offscreen)
        # A panel with nobody watching. The UI opens /dev/fb0 on the way up
        # and dies without one, so "none" cannot boot the phone at all --
        # but no window and no need for a desktop, which is what
        # smoke-testing a build wants.
        set -- "$@" \
            -device virtio-gpu-device \
            -device virtio-keyboard-device \
            -display none \
            -serial "$CONSOLE"
        APPEND="$APPEND video=vfb:on"
        ;;
    vnc)
        # The panel over the wire: no desktop needed on the machine running
        # it. What `offscreen` is for smoke tests, this is for driving the
        # thing -- over ssh, in a container, on a build box.
        #
        #     NEODCT_DISPLAY=vnc neodct/tools/run_qemu.sh
        #     vncviewer localhost:5901          (display :1 is port 5901)
        #
        # Until Stage 3 the viewer shows the virtio-gpu's blank scanout and
        # not the phone -- the pixels are in vfb. What it is good for today
        # is typing: keystrokes reach /dev/input/event0 from here.
        #
        # THE ADDRESS DEFAULTS TO LOOPBACK, DELIBERATELY. `-vnc :1` on its own
        # binds every interface, and a QEMU VNC server has no password unless
        # one is configured -- so the bare form publishes an interactive
        # console, as root, to the whole network. For a remote box the right
        # move is an ssh tunnel to the loopback listener:
        #
        #     ssh -L 5901:127.0.0.1:5901 the-box
        #
        # NEODCT_VNC overrides it if you really do want to listen wider; it is
        # passed to -vnc verbatim, so NEODCT_VNC="0.0.0.0:1,password=on" and
        # the like work.
        #
        # virtio-tablet-device matters here in a way it does not for gtk: VNC
        # sends absolute pointer positions, and without a tablet QEMU has to
        # guess at relative motion, which puts the cursor nowhere near where
        # you clicked.
        #
        # -serial stdio still applies, so the console is in the terminal you
        # started it from while the panel is in the viewer. That combination
        # is the reason to prefer this over gtk even where a desktop exists.
        set -- "$@" \
            -device virtio-gpu-device \
            -device virtio-keyboard-device \
            -device virtio-tablet-device \
            -vnc "$VNC_ADDR" \
            -serial "$CONSOLE"
        APPEND="$APPEND video=vfb:on"
        ;;
    *)
        set -- "$@" \
            -device virtio-gpu-device \
            -device virtio-keyboard-device \
            -device virtio-tablet-device \
            -display "$DISPLAY_MODE,gl=off,zoom-to-fit=off" \
            -serial "$CONSOLE"
        APPEND="$APPEND video=vfb:on"
        ;;
esac

# --- the panel transcript -------------------------------------------------
#
# One block after the case rather than a line in each of three, and after it
# rather than inside it because a shell function has its own "$@" and cannot
# append to the caller's argument list.
#
# `none` is REFUSED rather than ignored. It attaches no framebuffer at all --
# no `video=vfb:on`, so no /dev/fb0 -- and the daemon that would write this
# transcript exits without one. A flag that silently produced an empty file
# would look exactly like a broken panel.
#
# `neodct.panel=stream` is what S90display reads, and the port existing is
# deliberately NOT enough on its own: the daemon's backend is chosen and never
# detected, and a machine that grew a serial port is not a machine that asked
# for a continuous copy of its screen.
if [ -n "$PANEL_STREAM" ]; then
    if [ "$DISPLAY_MODE" = none ]; then
        echo "run_qemu: NEODCT_PANEL_STREAM needs a framebuffer, and" >&2
        echo "run_qemu: NEODCT_DISPLAY=none attaches none. Use offscreen." >&2
        exit 1
    fi
    # -chardev file truncates and writes; the guest only ever writes to the
    # port, so there is no reader to starve and no flow control to get wrong.
    set -- "$@" \
        -chardev "file,id=ndpanelchr,path=$PANEL_STREAM" \
        -device virtio-serial-device \
        -device "virtserialport,chardev=ndpanelchr,name=neodct.panel"
    APPEND="$APPEND neodct.panel=stream"
    echo "run_qemu: panel transcript -> $PANEL_STREAM" >&2
    echo "run_qemu:   decode it with neodct/tools/st7789_replay.py --out a.png" >&2
fi

# --- the keypad -----------------------------------------------------------
#
# THE PHONE'S KEYPAD IS A PCF8575 ON /dev/i2c-3, and until this landed the
# emulator had no i2c bus of any kind, so nd_pcf8575.c, nd_matrix.c, the T9
# engine's whole surround, nd_keypadsetup.c's 1,202 lines and
# apps/KeypadMapperI2C had run on exactly one machine in the world. The bus is
# a `vhost-user-i2c-device` whose transfers are serviced OUTSIDE QEMU by
# neodct/tools/nd-i2c-keypadd, which models the expander at 0x20 and the
# MAX17048 fuel gauge at 0x36 from their datasheets. The guest sees a real
# adapter, a real /dev/i2c-3 and real ioctls.
#
# THE VIRTIO KEYBOARD STAYS. It is not a QEMU shim standing in for the
# keypad: nd_input_open() opens the matrix FIRST and then opens an evdev
# device REGARDLESS, and polls both when both are present, because "a
# developer with a USB keyboard plugged into a real phone can still type".
# Deleting it to tidy up the emulator would remove a hardware feature to fix
# an emulator problem -- and a machine with both is the first one anywhere on
# which nd_input's backend SELECTION runs at all.
#
# WHICH IS WHY IT SAYS WHICH ONE WILL WIN, on every boot. A silent fast path
# is how the storage divergence came back last time.
#
# NEODCT_KEYPAD=off takes the bus away, and the fallback it leaves behind is
# named rather than implied. It is a real escape hatch and not a courtesy:
# QEMU REFUSES TO START when the vhost-user socket is not there -- measured,
# `Failed to connect to '...': No such file or directory' -- so a host with a
# QEMU that has no vhost-user-i2c-device would otherwise have no emulator at
# all rather than an emulator with no keypad.
#
# ============ ONE BASE, THREE PATHS, SO TWO SESSIONS CAN BE TWO ============
#
# The fifo keeps its documented default -- AGENTS.md tells people to
# `echo 'tap num_5' > /tmp/neodct-keys` and that must stay true -- and the
# socket and the log are DERIVED FROM IT rather than being two more fixed
# per-host names. So `NEODCT_KEYS=/tmp/keys-b run_qemu.sh` is a genuinely
# separate second session, where before it shared a socket and a log with the
# first one and the two daemons split the keystrokes between them (measured:
# twelve presses went 9/3). The daemon refuses a socket somebody is already
# serving rather than taking it, so the collision is now loud either way --
# including the orphan case, where a session whose terminal died hard leaves a
# daemon still holding the socket and still polling the fifo.
KEYS_FIFO="${NEODCT_KEYS:-${TMPDIR:-/tmp}/neodct-keys}"
KEYPADD_SOCK="$KEYS_FIFO.sock"
KEYPADD_LOG="$KEYS_FIFO.log"
# ONE SHAREABLE BACKEND, NOT TWO. The NEODCT_SD=share path attaches its own
# `-object memory-backend-file ... -numa node,memdev=mem` for vhost-user-fs,
# and a second backend claiming to be the guest's RAM is a QEMU start-up
# error, not a fallback. That combination is unreachable today -- NEODCT_SD=share
# is refused two hundred lines up for want of CONFIG_VIRTIO_FS -- and it is
# named here anyway, because the day somebody adds that symbol back the
# failure would be an unexplained QEMU error in a mode nobody had changed.
if [ "$SD_MODE" = "share" ] && [ -z "$KEYPAD_NODTB" ] && \
   [ "$KEYPAD" != "off" ] && [ "$KEYPAD" != "none" ] && [ "$KEYPAD" != "0" ]; then
    echo "run_qemu: NEODCT_SD=share and the i2c keypad both need the guest's RAM" >&2
    echo "  to be a shareable backend, and QEMU takes only one. Boot with" >&2
    echo "  NEODCT_KEYPAD=off, or teach nd_qemu_i2c_args() to reuse the backend" >&2
    echo "  the share path already attaches." >&2
    exit 1
fi

# An `if` and NOT `[ -n ... ] && KEYPAD=off`: under `set -e` a false test as
# the last command of an AND-list terminates the script, which is the trap
# this file's own cleanup comments were written about.
if [ -n "$KEYPAD_NODTB" ]; then
    KEYPAD=off
fi

case "$KEYPAD" in
    off|none|0)
        if [ -n "$KEYPAD_NODTB" ]; then
            echo "run_qemu: and with no device tree there is no i2c bus at all," >&2
            echo "  so NONE of this runs: the PCF8575 driver, the matrix" >&2
        else
            echo "run_qemu: NEODCT_KEYPAD=off -- no i2c bus, so /dev/i2c-3 does not" >&2
            echo "  exist and NONE of this runs: the PCF8575 driver, the matrix" >&2
        fi
        echo "  scanner, the keymap loader, the first-boot keypad wizard, the" >&2
        echo "  root-phase bring-up that hands a descriptor across the privilege" >&2
        echo "  drop, T9 and its two uinput bridges, KeypadMapperI2C, and" >&2
        echo "  nd_battery.c's live MAX17048 path. Keys come from the QEMU" >&2
        echo "  window through /dev/input/event0 instead, which is the machine" >&2
        echo "  the emulator was before this stage." >&2
        ;;
    *)
        if KEYPAD_ARGS=$(nd_qemu_i2c_args "$KEYPADD_SOCK" "$MEMORY"); then
            if KEYPADD_PID=$(nd_keypadd_start "$HERE" "$KEYPADD_SOCK" "$KEYS_FIFO" \
                                              "$KEYPADD_LOG"); then
                # shellcheck disable=SC2086  # deliberately word-split
                set -- "$@" $KEYPAD_ARGS
                echo "run_qemu: i2c keypad on /dev/i2c-3 (PCF8575 0x20, MAX17048 0x36)." >&2
                echo "run_qemu:   type into it with:  echo 'tap num_5' > $KEYS_FIFO" >&2
                echo "run_qemu:   press/release/tap <key>, release all, short <pinA> <pinB>" >&2
                echo "run_qemu:   the daemon's log is $KEYPADD_LOG" >&2
                # ============ AND THE WIZARD, WHICH IS NEW ON THIS MACHINE ===
                #
                # nd_kpsetup_gate_check() returns PROBE the moment /dev/i2c-3
                # exists -- the is_hw test is only reached when the node is
                # ABSENT -- and nd_main.c calls nd_kpsetup_maybe_run() on every
                # boot. No keymap.json ships in the overlay; it lives on
                # /NeoDCT/User, which the wizard writes. So a fresh userdata
                # partition now stops at the sixteen-prompt enrolment screen
                # where this machine used to go QUIET, and an operator watching
                # for `Input backend selected:` has no reason to know why.
                #
                # It is said UNCONDITIONALLY, and not gated on "does the
                # userdata have a keymap": /NeoDCT/User is a ubifs volume
                # inside a nandsim image and there is no host-side way to look
                # inside one -- post-image-neodct.sh says as much about
                # NEODCT_KEEP_USERDATA. A test this script cannot actually
                # perform is worse than a paragraph that is true every time.
                echo "run_qemu: a /NeoDCT/User with no keymap.json now stops at the" >&2
                echo "  SIXTEEN-PROMPT first-boot keypad wizard before the home screen." >&2
                echo "  That is new on this machine: nd_kpsetup_gate_check() returns" >&2
                echo "  PROBE as soon as /dev/i2c-3 exists, and it never did here" >&2
                echo "  before. Clear it by feeding the pad in enrolment order:" >&2
                echo "    for k in navikey clear up down num_1 num_2 num_3 num_4 \\" >&2
                echo "             num_5 num_6 num_7 num_8 num_9 num_0 star hash; do" >&2
                echo "      echo \"tap \$k\" > $KEYS_FIFO; sleep 1; done" >&2
                echo "  NEODCT_SNAPSHOT=1 and NEODCT_STORAGE=nand-full both discard" >&2
                echo "  /NeoDCT/User, so under either the wizard runs on EVERY boot." >&2
                echo "run_qemu: the virtio keyboard is still attached, so nd_input picks" >&2
                echo "  the MATRIX and keeps evdev as the second backend -- which is the" >&2
                echo "  phone's own arrangement. Watch for 'Input backend selected:' in" >&2
                echo "  the boot log; if it says evdev, the matrix did not open and the" >&2
                echo "  line says why." >&2
            else
                KEYPADD_PID=""
                echo "run_qemu: the keypad daemon would not start, so no i2c bus this" >&2
                echo "  boot. $KEYPADD_LOG has the reason. Booting on evdev." >&2
            fi
        else
            echo "run_qemu: no i2c keypad this boot (see the line above). The" >&2
            echo "  emulator falls back to the QEMU keyboard, which is a different" >&2
            echo "  input path from the phone's." >&2
        fi
        ;;
esac

# --- usb: audio, and optionally the real modem ---------------------------
# Nothing below runs today: AUDIO is none and NEODCT_MODEM / NEODCT_BT refuse
# at the top, because qemu-xhci is a PCI device and the guest has no PCI. It
# is kept as it stood so that restoring the symbol restores the feature.
NEED_XHCI=""
[ "$AUDIO" != "none" ] && NEED_XHCI=1
[ -n "${NEODCT_MODEM:-}" ] && NEED_XHCI=1
[ -n "${NEODCT_BT:-}" ] && NEED_XHCI=1
[ -n "$NEED_XHCI" ] && set -- "$@" -device qemu-xhci,id=xhci

if [ "$AUDIO" != "none" ]; then
    set -- "$@" \
        -audiodev "$AUDIO,id=audio0,in.mixing-engine=off,out.mixing-engine=off" \
        -device usb-audio,bus=xhci.0,audiodev=audio0
fi

if [ -n "${NEODCT_MODEM:-}" ]; then
    # Needs the SIM7600 plugged in and readable (udev rule or root).
    set -- "$@" -device \
        "usb-host,bus=xhci.0,vendorid=$MODEM_VENDOR,productid=$MODEM_PRODUCT"
fi

if [ -n "${NEODCT_BT:-}" ]; then
    # Same deal as the modem: QEMU has to OPEN the dongle's /dev/bus/usb node
    # read-write, and those nodes are crw-rw-r-- root:root. Without a rule
    # QEMU fails with "libusb: bad access (-3)" and the guest simply sees no
    # USB device -- no error reaches the phone, so the symptom is a Bluetooth
    # app that says "no controller" for a reason nothing on screen can
    # explain. The durable fix is a udev rule; docs/BLUETOOTH.md has it.
    #
    # QEMU's own emulated Bluetooth stack is NOT an alternative: it was
    # removed in QEMU 6.0 and this host runs 11.0.3. For an hci device with
    # no dongle at all, the kernel's CONFIG_BT_HCIVHCI is the way in.
    set -- "$@" -device \
        "usb-host,bus=xhci.0,vendorid=$BT_VENDOR,productid=$BT_PRODUCT"
fi

# --- networking (off by default, as in the modem test invocation) --------
if [ -n "${NEODCT_NET:-}" ]; then
    set -- "$@" -netdev user,id=eth0 -device virtio-net-device,netdev=eth0
else
    # No NIC unless one was asked for. Without this QEMU still creates a
    # default user-mode NIC, and on a kernel that can drive one the guest
    # gets an eth0 with its own IPv6 default route at the same metric as the
    # modem's:
    #
    #   default via fe80::2                   dev eth0        metric 1024
    #   default via fe80::e147:b5cd:41f5:a46f dev wwp0s2u2i5  metric 1024
    #
    # eth0 wins, and every packet the modem should carry goes to slirp
    # instead. It also cost 15s a boot waiting for DHCP on a NIC that
    # leads nowhere real. Neither can happen on this kernel, which has no
    # NETDEVICES -- this stays because it is the setting that is still right
    # on the day they come back.
    set -- "$@" -nic none
fi

# --- virtiofs share as the card -----------------------------------------
if [ "$SD_MODE" = "share" ]; then
    # A host folder appears as the card. Handy for dropping in an
    # UPDATE.ndsw, but it is not a block device and has no FAT label, so the
    # card detection and format flows cannot be exercised this way.
    VIRTIOFSD="${NEODCT_VIRTIOFSD:-/usr/lib/virtiofsd}"
    SOCKET="${NEODCT_VFS_SOCK:-/tmp/claude-1000/nsq-virtiofs.sock}"
    if [ ! -x "$VIRTIOFSD" ]; then
        echo "run_qemu: $VIRTIOFSD not found; set NEODCT_VIRTIOFSD" >&2
        exit 1
    fi
    mkdir -p "$SHARE_DIR" "$(dirname "$SOCKET")"
    for folder in wallpapers tones backup_db music update; do
        mkdir -p "$SHARE_DIR/$folder"
    done
    rm -f "$SOCKET"
    echo "run_qemu: sharing $SHARE_DIR as the SD card"
    "$VIRTIOFSD" --socket-path="$SOCKET" --shared-dir "$SHARE_DIR" \
        --sandbox=none > /tmp/virtiofsd.log 2>&1 &
    # No trap of its own any more, and that is the fix rather than tidying:
    # this one replaced the NAND cleanup installed above it, so a share boot
    # saved nothing and left its work directory behind. nd_session_end() kills
    # it, and there is one EXIT trap in this file.
    VIRTIOFSD_PID=$!
    tries=0
    while [ ! -S "$SOCKET" ] && [ "$tries" -lt 50 ]; do
        sleep 0.1
        tries=$((tries + 1))
    done
    set -- "$@" \
        -chardev "socket,id=ndsdfs,path=$SOCKET" \
        -device vhost-user-fs-device,queue-size=1024,chardev=ndsdfs,tag=neodct-sd
    APPEND="$APPEND neodct.sdshare=neodct-sd"
    # vhost-user needs the guest's memory to be shareable.
    set -- "$@" -object "memory-backend-file,id=mem,size=${MEMORY}M,mem-path=/dev/shm,share=on" \
        -numa node,memdev=mem
fi

[ -n "$MONITOR" ] && set -- "$@" -monitor "unix:$MONITOR,server,nowait"
# -snapshot makes every drive copy-on-write, the cache file included, so a
# snapshot boot has nothing to save and the block below skips it. That is the
# same meaning NEODCT_SNAPSHOT has always had.
[ -n "${NEODCT_SNAPSHOT:-}" ] && set -- "$@" -snapshot

# ============ WHAT THIS BOOT IS NOT TESTING ============
#
# One line, on the boot that skips the phone's storage. A banner is the
# WEAKEST of the four things that stop the fast path from becoming the way
# everybody works, and it is here last on purpose. The other three are
# stronger and need nobody to read anything:
#
#   * an nd-inventory capture taken on NEODCT_STORAGE=virtio is visibly a
#     different machine -- class.ubi is [version] instead of
#     [ubi1 ubi1_0 version], the ubi.* records are absent and /NeoDCT/User is
#     ext4 rather than ubifs -- so parity_capture_qemu.sh --compare fails on
#     it with no new machinery;
#   * parity_capture_qemu.sh refuses such a capture outright rather than
#     letting it reach a diff, the way it already refuses one taken without
#     libneodct;
#   * verity_state.prop already records user_device=, and on this path it says
#     /dev/vdX where the NAND path says ubi1:userdata. Nothing new to write,
#     something new to read.
if [ "$STORAGE" = "virtio" ]; then
    echo "run_qemu: NEODCT_STORAGE=virtio -- /NeoDCT/User is ext4 on a virtio" >&2
    echo "  disk, which the phone does not have. user_is_ubi(), the ubifs" >&2
    echo "  mount and mknand.sh's userdata.ubi are NOT exercised by this boot," >&2
    echo "  and a capture taken on it cannot be a parity baseline." >&2
fi

# The other silent cost, and the banner was on the wrong mode. virtio says
# what it is not exercising; nand-full says nothing at all and DISCARDS the
# session -- it deliberately has no cache file (a ubiblock read through one
# deadlocks the guest), so there is no host-side file for nd_nand_save() to
# lift /NeoDCT/User out of. An hour of work in the guest, a clean poweroff,
# and the partition is back at the factory skeleton next boot. The mode that
# keeps writes was the one with the warning.
if [ "$STORAGE" = "nand-full" ]; then
    echo "run_qemu: NEODCT_STORAGE=nand-full -- this mode has NO nandsim cache" >&2
    echo "  file, because a ubiblock read through one deadlocks the guest, so" >&2
    echo "  nothing lifts /NeoDCT/User back out when QEMU exits: everything" >&2
    echo "  written to the user partition this session is discarded. Use the" >&2
    echo "  default NEODCT_STORAGE=nand for anything you want to keep." >&2
fi
if [ "$STORAGE" != "virtio" ] && [ -n "${NEODCT_SNAPSHOT:-}" ]; then
    echo "run_qemu: NEODCT_SNAPSHOT=1 -- every drive is copy-on-write, the" >&2
    echo "  nandsim cache file included, so /NeoDCT/User is not saved when" >&2
    echo "  QEMU exits. That is what snapshot has always meant here." >&2
fi

# ============ AND WHY THIS IS NOT AN exec ============
#
# It was, for the whole life of this script. A NAND boot has to lift
# /NeoDCT/User back out of the cache file after QEMU exits, or the partition
# resets on every boot -- which would silently make every session a snapshot
# session and kill the stage-reboot-apply workflow this storage stack exists
# for.
#
# THE CONDITION IS "IS THERE ANYTHING LEFT TO DO", AND IT USED TO BE "IS THIS
# THE SAVING MODE", which was three leaks in one line. NAND_WORK is built for
# every non-virtio mode and holds the flasher overlay, a copy of the whole
# initramfs and a 132 MB sparse cache file, and an EXIT trap does not survive
# an exec -- so `nand-full` and `NEODCT_SNAPSHOT=1` each left one of those
# directories in TMPDIR on every successful boot, for ever. The share path
# leaked a virtiofsd the same way. So: exec only when this script genuinely
# has nothing to clean up and nothing to save, which is the plain virtio boot
# with no host folder attached -- the mode that never made a NAND_WORK.
# Everywhere else, an extra shell in the process tree is the price of the
# cleanup, and it now forwards a kill rather than orphaning QEMU under one.
#
# shellcheck disable=SC2086  # EXTRA is intentionally word-split
# ...and a keypad daemon is a third thing to clean up, so it joins the two
# above in the list of reasons not to exec. An exec'd shell has no EXIT trap.
if [ "$STORAGE" = "virtio" ] && [ "$SD_MODE" != "share" ] && [ -z "$KEYPADD_PID" ]; then
    trap - EXIT INT TERM
    exec qemu-system-arm "$@" -append "$APPEND" $EXTRA
fi

# `<&3` and not plain `&`: an asynchronous command with no explicit
# redirection gets /dev/null on stdin, and stdin is the serial console. fd 3
# was saved at the top of the file, before any of that could apply.
QEMU_RAN=1
qemu-system-arm "$@" -append "$APPEND" $EXTRA <&3 &
QEMU_PID=$!

# ============ A DEAD KEYPAD DAEMON IS A FROZEN GUEST, NOT A DEAD KEYPAD =====
#
# QEMU's refusal to start against a missing socket only covers t=0. AFTER it
# has connected, i2c-virtio waits in wait_for_completion_interruptible() with
# the i2c bus lock held and there is NO TIMEOUT anywhere in that path, so a
# daemon that segfaults or is OOM-killed leaves every later transfer
# outstanding for ever. Measured: kill -9 the daemon at scan pass 5 and the
# guest never printed pass 6 and never reached its end marker; QEMU exited
# only because a `timeout` shot it. On a real image that read is
# nd_input_read_key() -> nd_matrix_scan_once(), i.e. the UI's own key loop,
# and nd_battery's poll blocks behind the same bus lock -- so the whole
# emulator stops with nothing on the console saying why.
#
# So: watch the daemon, and turn a silent freeze into a message and an exit.
# It polls rather than trapping SIGCHLD because the daemon is not this shell's
# only child and a trap would have to work out which one went.
if [ -n "$KEYPADD_PID" ]; then
    ( while kill -0 "$KEYPADD_PID" 2>/dev/null; do sleep 1; done
      kill -0 "$QEMU_PID" 2>/dev/null || exit 0
      echo "" >&2
      echo "run_qemu: THE KEYPAD BACKEND DIED WITH THE GUEST RUNNING." >&2
      echo "  i2c-virtio waits on a completion with the bus lock held and no" >&2
      echo "  timeout, so the guest is frozen in its key loop rather than" >&2
      echo "  running without a keypad. Stopping QEMU. Last of $KEYPADD_LOG:" >&2
      tail -5 "$KEYPADD_LOG" >&2 2>/dev/null || true
      kill "$QEMU_PID" 2>/dev/null || true ) &
    KEYPADD_WATCH_PID=$!
fi

QEMU_STATUS=0
wait "$QEMU_PID" || QEMU_STATUS=$?
QEMU_PID=""

# The save and the cleanup are nd_session_end()'s, on the EXIT trap, so that
# they happen identically whether QEMU returned 0, returned 1 or was killed.
exit "$QEMU_STATUS"

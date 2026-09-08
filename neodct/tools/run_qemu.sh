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
# TWO THINGS ARE STILL MISSING AND BOTH ARE THE PANEL STAGE'S:
#
# vfb comes up at its built-in default -- 640x480 at 8 bpp, measured here --
# and is put into 240x175x32 by a userspace FBIOPUT_VSCREENINFO. On the phone
# neodct_displayd does that;
# under QEMU nothing does it yet, so nd_fb sees 640x480x8, takes it, and
# writes a 240-wide band into a 640-wide 8-bit buffer. Measured with a probe
# doing exactly what displayd's force_mode() does: after the ioctl it is
# 240x175, 32 bpp, line_length 960, red.offset 0 -- the phone's framebuffer
# byte for byte. So the fix is to run the phone's own code path, which also
# makes force_mode() a tested path for the first time.
#
# And vfb has no scanout, so nothing a QEMU display frontend shows is the
# phone. The virtio-gpu device below is still attached, and not for the
# picture: a frontend only routes keystrokes into the guest's virtio keyboard
# when console 0 is a GRAPHIC console. Measured -- with no graphics device at
# all, keys sent over VNC went to QEMU's text console and /dev/input/event0
# saw none of them; with virtio-gpu-device they arrive. With
# NEODCT_DISPLAY=none there is no frontend and the way in is the monitor:
# NEODCT_MONITOR=/tmp/ndmon, then `sendkey a` (also measured).
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
for required in zImage initramfs.cpio.gz system.img userdata.ext4; do
    if [ ! -f "$IMAGES/$required" ]; then
        echo "run_qemu: $IMAGES/$required missing." >&2
        echo "  Build with: cd buildroot && make neodct_qemu_defconfig && make" >&2
        exit 1
    fi
done

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
    NDDTB=""
fi

set -- \
    -M virt \
    -cpu cortex-a7 \
    -smp 1 \
    -m "$MEMORY" \
    -global virtio-mmio.force-legacy=false \
    -kernel "$IMAGES/zImage" \
    -initrd "$IMAGES/initramfs.cpio.gz" \
    -drive "file=$IMAGES/system.img,if=none,format=raw,id=ndsys" \
    -device virtio-blk-device,drive=ndsys,serial=NDSYS \
    -drive "file=$IMAGES/userdata.ext4,if=none,format=raw,id=nduser" \
    -device virtio-blk-device,drive=nduser,serial=NDUSER

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
    VIRTIOFSD_PID=$!
    trap 'kill $VIRTIOFSD_PID 2>/dev/null || true' EXIT INT TERM
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
[ -n "${NEODCT_SNAPSHOT:-}" ] && set -- "$@" -snapshot

# shellcheck disable=SC2086  # EXTRA is intentionally word-split
exec qemu-system-arm "$@" -append "$APPEND" $EXTRA

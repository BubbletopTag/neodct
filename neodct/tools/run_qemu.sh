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
#   MemTotal       53,824 kB of the 64 MB machine; the phone has ~54 MB
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
# kernel. The default used to be 72 "to stay near the Pico Mini's 64MB",
# which was a fudge for a kernel fat enough that 64 would have been 12 MB
# HARSHER than the hardware. It is not needed any more, and a fudge that says
# 72 while the phone says 64 is the reason a build fits here and not there.
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
APPEND="console=ttyAMA0 vt.global_cursor_default=0 neodct.verity=$VERITY"
APPEND="$APPEND mtdram.total_size=0"
APPEND="$APPEND nandsim.first_id_byte=0x20 nandsim.second_id_byte=0xa1"
APPEND="$APPEND nandsim.third_id_byte=0x00 nandsim.fourth_id_byte=0x15"
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
        set -- "$@" -nographic
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
            -serial stdio
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
            -serial stdio
        APPEND="$APPEND video=vfb:on"
        ;;
    *)
        set -- "$@" \
            -device virtio-gpu-device \
            -device virtio-keyboard-device \
            -device virtio-tablet-device \
            -display "$DISPLAY_MODE,gl=off,zoom-to-fit=off" \
            -serial stdio
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

#!/bin/sh
# Boot the immutable NeoDCT image set under QEMU.
#
# Device set and cmdline follow the known-good invocation used for the
# ext4 image (virtio *-pci* for gpu/keyboard/tablet, virtio-blk-device for
# storage, xhci + usb-audio, 240x175M framebuffer, 72MB of RAM to stay near
# the Pico Mini's 64MB). What changed for the immutable layout:
#
#   * no root= -- the initramfs mounts everything and switch_roots
#   * three virtio-blk devices instead of one, in this order:
#       /dev/vda  system.img     squashfs + dm-verity tree, becomes /
#       /dev/vdb  userdata.ext4  /NeoDCT/User
#       /dev/vdc  sdcard.img     the removable card (FAT32, label NEODCT)
#   * neodct.sys / neodct.user / neodct.verity on the cmdline
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
#   NEODCT_SD=share ...                       host folder as the card (virtiofs)
#   NEODCT_SD=none ...                        no card inserted
#   NEODCT_RECOVERY=1 ...                     boot into recovery mode
#   NEODCT_RECTTY=/dev/console ...            drive recovery over serial
#   NEODCT_MODEM=1 ...                        pass the SIM7600 through
#   NEODCT_BT=1 ...                           pass the UB500 dongle through
#   NEODCT_NET=1 ...                          add a virtio NIC (browser testing)
#   NEODCT_AUDIO=none ...                     no audio device (the default when
#                                             no PulseAudio server is running)
#   NEODCT_AUDIO=pa ...                       force PulseAudio even if the
#                                             probe below found no server
#   NEODCT_DISPLAY=none ...                   no panel at all (the UI cannot
#                                             boot; serial/recovery only)
#   NEODCT_DISPLAY=offscreen ...              panel present, no window
#   NEODCT_DISPLAY=vnc ...                    panel over VNC, no desktop needed
#                                             (127.0.0.1:5901; NEODCT_VNC to move it)
#   NEODCT_MONITOR=/tmp/ndmon ...             QEMU monitor socket -- screendump
#
# Persistence matters for update testing: SystemUpdate stages an update, the
# phone reboots and the initramfs applies it. With NEODCT_SNAPSHOT=1 that
# write is discarded and the update looks like it vanished.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
IMAGES="${NEODCT_IMAGES:-$(dirname "$REPO")/buildroot/output/images}"

MEMORY="${NEODCT_MEM:-72}"
VERITY="${NEODCT_VERITY:-enforce}"
SD_MODE="${NEODCT_SD:-image}"
DISPLAY_MODE="${NEODCT_DISPLAY:-gtk}"
SHARE_DIR="${NEODCT_SHARE:-$HOME/neodct-sdcard}"
MONITOR="${NEODCT_MONITOR:-}"

# Where a vnc display listens. See the `vnc` case below for why the default
# is a loopback address and not a bare ":1".
VNC_ADDR="${NEODCT_VNC:-127.0.0.1:1}"
EXTRA="${NEODCT_QEMU_EXTRA:-}"

# ============ WHY THE AUDIO BACKEND IS PROBED AND NOT ASSUMED ============
#
# This used to be plain `${NEODCT_AUDIO:-pa}`, and on any machine without a
# PulseAudio daemon QEMU refuses to start AT ALL -- not "the phone boots
# silently", but two fatal errors before the kernel is even loaded:
#
#     qemu-system-aarch64: XDG_RUNTIME_DIR not set
#     qemu-system-aarch64: could not stat pidfile /run/xdg/pulse/pid
#
# Neither says the word "audio", so the obvious reading is that the image or
# the script is broken. Setting XDG_RUNTIME_DIR by hand only advances it from
# the first error to the second, because the real problem is that there is no
# sound server behind the socket.
#
# That is every headless box: a CI runner, a container, a VPS over SSH -- and
# a phone emulator with no speakers is worth far more than one that will not
# boot. So the default is now "pa if something is actually listening,
# otherwise none".
#
# An EXPLICIT NEODCT_AUDIO is still obeyed verbatim, `pa` included: someone
# debugging their own audio setup wants QEMU's real complaint, not this
# script's guess.
pick_audio() {
    # PULSE_SERVER wins wherever it is set -- it can name a TCP server or a
    # socket somewhere else entirely, and probing the default path would
    # wrongly conclude there is nothing there.
    [ -n "${PULSE_SERVER:-}" ] && { echo pa; return; }
    for sock in "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pulse/native" \
                "/run/user/$(id -u)/pulse/native"; do
        [ -S "$sock" ] && { echo pa; return; }
    done
    echo none
}

if [ -n "${NEODCT_AUDIO:-}" ]; then
    AUDIO="$NEODCT_AUDIO"
else
    AUDIO="$(pick_audio)"
    [ "$AUDIO" = none ] && printf '%s\n' \
        "run_qemu: no PulseAudio server; starting without audio." \
        "run_qemu: set NEODCT_AUDIO=pa to force it and see QEMU's own error." >&2
fi

# SIM7600 as seen on the USB bus.
MODEM_VENDOR="${NEODCT_MODEM_VENDOR:-0x1e0e}"
MODEM_PRODUCT="${NEODCT_MODEM_PRODUCT:-0x9001}"

# TP-Link UB500, an RTL8761BU. Overridable because any btusb-class dongle
# works: btusb matches on the USB class (e0/01/01), not on the id.
BT_VENDOR="${NEODCT_BT_VENDOR:-0x2357}"
BT_PRODUCT="${NEODCT_BT_PRODUCT:-0x0604}"

for required in Image initramfs.cpio.gz system.img userdata.ext4; do
    if [ ! -f "$IMAGES/$required" ]; then
        echo "run_qemu: $IMAGES/$required missing." >&2
        echo "  Build with: cd buildroot && make neodct_qemu_defconfig && make" >&2
        exit 1
    fi
done

# serial= is what makes the drives identifiable. QEMU does not enumerate
# virtio-mmio devices in the order they are given here -- system, user, card
# comes up in the guest as vda=card, vdb=user, vdc=system -- so neodct.sys
# below is only ever a hint. The initramfs reads the serial back out of
# /sys/block/*/serial, which works even after "wipe system" has zeroed the
# image and left nothing on the device to recognise it by.
set -- \
    -M virt \
    -cpu cortex-a53 \
    -m "$MEMORY" \
    -kernel "$IMAGES/Image" \
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
        else
            echo "run_qemu: no sdcard.img; booting with no card" >&2
        fi
        ;;
    share) ;;   # set up after the cmdline is assembled, needs a tag
    none)  ;;
    *)
        echo "run_qemu: NEODCT_SD must be image, share or none" >&2
        exit 1
        ;;
esac

# --- kernel cmdline ------------------------------------------------------
# Two consoles as before; vt.global_cursor_default=0 keeps the VT cursor off
# the UI, and quiet/loglevel=0 keep kernel messages from drawing over it.
APPEND="console=ttyAMA0 console=ttyS0,115200 vt.global_cursor_default=0"
APPEND="$APPEND neodct.sys=/dev/vda neodct.user=/dev/vdb neodct.verity=$VERITY"
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

# Anything else you want on the kernel command line, appended last so it wins.
# The reason this exists: printk.time=1. CONFIG_PRINTK_TIME is off in both
# kernel configs, so a boot log has no timestamps at all and "which part of
# the boot is slow" cannot be answered without rebuilding the kernel.
#
#     NEODCT_APPEND="printk.time=1 initcall_debug" NEODCT_DEBUG=1 run_qemu.sh
[ -n "${NEODCT_APPEND:-}" ] && APPEND="$APPEND $NEODCT_APPEND"

# --- display -------------------------------------------------------------
case "$DISPLAY_MODE" in
    none)
        set -- "$@" -nographic
        ;;
    offscreen)
        # A panel with nobody watching. Same devices as a windowed run --
        # the UI opens /dev/fb0 on the way up and dies without one, so
        # "none" cannot boot the phone at all -- but no window and no need
        # for a desktop, which is what smoke-testing a build wants.
        set -- "$@" \
            -device virtio-gpu-pci \
            -device virtio-keyboard-pci \
            -display none \
            -serial stdio
        APPEND="$APPEND video=Virtual-1:240x175M"
        ;;
    vnc)
        # The panel over the wire: a real, clickable phone with no desktop on
        # the machine running it. What `offscreen` is for smoke tests, this is
        # for actually using the thing -- over ssh, in a container, on a build
        # box.
        #
        #     NEODCT_DISPLAY=vnc neodct/tools/run_qemu.sh
        #     vncviewer localhost:5901          (display :1 is port 5901)
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
        # virtio-tablet-pci matters here in a way it does not for gtk: VNC
        # sends absolute pointer positions, and without a tablet QEMU has to
        # guess at relative motion, which puts the cursor nowhere near where
        # you clicked.
        #
        # -serial stdio still applies, so the console is in the terminal you
        # started it from while the panel is in the viewer. That combination
        # is the reason to prefer this over gtk even where a desktop exists.
        set -- "$@" \
            -device virtio-gpu-pci \
            -device virtio-keyboard-pci \
            -device virtio-tablet-pci \
            -vnc "$VNC_ADDR" \
            -serial stdio
        APPEND="$APPEND video=Virtual-1:240x175M"
        ;;
    *)
        set -- "$@" \
            -device virtio-gpu-pci \
            -device virtio-keyboard-pci \
            -device virtio-tablet-pci \
            -display "$DISPLAY_MODE,gl=off,zoom-to-fit=off" \
            -serial stdio
        # The UI band is 240x175; on QEMU the framebuffer is that size
        # directly, with no 240x240 panel to letterbox into.
        APPEND="$APPEND video=Virtual-1:240x175M"
        ;;
esac

# --- usb: audio, and optionally the real modem ---------------------------
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
    # default user-mode NIC, and the guest gets an eth0 with its own IPv6
    # default route at the same metric as the modem's:
    #
    #   default via fe80::2                   dev eth0        metric 1024
    #   default via fe80::e147:b5cd:41f5:a46f dev wwp0s2u2i5  metric 1024
    #
    # eth0 wins, and every packet the modem should carry goes to slirp
    # instead. It also cost 15s a boot waiting for DHCP on a NIC that
    # leads nowhere real.
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
exec qemu-system-aarch64 "$@" -append "$APPEND" $EXTRA

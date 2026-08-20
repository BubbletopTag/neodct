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
#   NEODCT_SD=share ...                       host folder as the card (virtiofs)
#   NEODCT_SD=none ...                        no card inserted
#   NEODCT_RECOVERY=1 ...                     boot into recovery mode
#   NEODCT_RECTTY=/dev/console ...            drive recovery over serial
#   NEODCT_MODEM=1 ...                        pass the SIM7600 through
#   NEODCT_NET=1 ...                          add a virtio NIC (browser testing)
#   NEODCT_AUDIO=none ...                     no audio device
#   NEODCT_DISPLAY=none ...                   headless, serial only
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
AUDIO="${NEODCT_AUDIO:-pa}"
SHARE_DIR="${NEODCT_SHARE:-$HOME/neodct-sdcard}"
MONITOR="${NEODCT_MONITOR:-}"
EXTRA="${NEODCT_QEMU_EXTRA:-}"

# SIM7600 as seen on the USB bus.
MODEM_VENDOR="${NEODCT_MODEM_VENDOR:-0x1e0e}"
MODEM_PRODUCT="${NEODCT_MODEM_PRODUCT:-0x9001}"

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

# --- display -------------------------------------------------------------
case "$DISPLAY_MODE" in
    none)
        set -- "$@" -nographic
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

# --- networking (off by default, as in the modem test invocation) --------
if [ -n "${NEODCT_NET:-}" ]; then
    set -- "$@" -netdev user,id=eth0 -device virtio-net-device,netdev=eth0
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

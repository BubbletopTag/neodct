# Bluetooth on NeoDCT

The phone has no Bluetooth radio of its own. Bluetooth arrives on USB, as a
TP-Link UB500 dongle (`2357:0604`), and everything below is about making that
dongle work: what the kernel needs, what firmware it needs, and what proves it
is working.

There is an engineering app for the last part -- **Bluetooth**, app id 9007,
in the engineering menu.

---

## The stack, and what is deliberately not in it

There is no `bluetoothd`, no D-Bus and no BlueZ userspace at all.

Everything the phone does with Bluetooth today -- enumerate controllers, read
the adapter's BD_ADDR, bring the radio up, run an inquiry -- is an `ioctl` on
an `AF_BLUETOOTH` socket, served by the kernel's own BlueZ core. So the whole
userspace side is `neodct/src/lib/nd_bt.c`, about 400 lines, linked into
`libneodct` and paged in only by the one app that calls it.

The alternative was `BR2_PACKAGE_BLUEZ5_UTILS`, which `select`s `dbus` and
`libglib2`. That is a message bus, a ~1.5 MB object library and a resident
daemon, added to a 64 MB device, for a feature that is engineering-only. It
buys nothing this app needs.

**When that argument stops holding:** pairing, Bluetooth audio (A2DP/HFP),
RFCOMM, or anything that has to hold a connection open. Those need an
event-driven HCI session and a bonding database, which is what `bluetoothd`
is. The moment one of them is wanted, add BlueZ and delete this paragraph.
The `hci_inquiry` ioctl carries no device name and no RSSI for the same
reason -- both need that session -- which is why a scan shows addresses and
device classes and not names.

---

## Kernel

`buildroot/board/qemu/aarch64-virt/linux.config`:

```
CONFIG_BT=y
CONFIG_BT_BREDR=y
CONFIG_BT_HCIBTUSB=y
CONFIG_BT_HCIBTUSB_RTL=y
CONFIG_BT_HCIVHCI=y          # QEMU only -- see "Testing without the dongle"
CONFIG_FW_LOADER=y
# CONFIG_BT_HCIBTUSB_BCM is not set
```

Three of those are worth a sentence.

**`CONFIG_BT_BREDR`** defaults to `y`, and is written out anyway because it is
load-bearing rather than decorative: `hci_inquiry()` in `hci_core.c` returns
`-EOPNOTSUPP` unless `HCI_BREDR_ENABLED` is set. Lose it and the adapter comes
up, reports its address, and the Scan page fails with "Operation not
supported" for a reason nothing on the screen explains.

**`CONFIG_BT_HCIBTUSB_RTL`** is what makes the UB500 work at all. `btusb`
binds it on the USB class alone -- interface class `e0`, subclass `01`,
protocol `01`, so there is no id-table entry to add -- but an RTL8761BU
carries no firmware. `btrtl` reads the controller's LMP subversion, matches
`RTL_ROM_LMP_8761A` / rev `0xb`,`0xa` on `HCI_USB`, and asks the firmware
loader for `rtl_bt/rtl8761bu_fw.bin` and `rtl_bt/rtl8761bu_config.bin`. If it
does not get them, the controller still registers -- `hci0` exists, the app's
ADAPTER step passes -- and its BD_ADDR is `00:00:00:00:00:00`. That specific
failure is why the app has a separate ADDRESS step.

**`CONFIG_BT_HCIBTUSB_BCM`** is turned off; `BT_INTEL` cannot be, because
`BT_HCIBTUSB` selects it unconditionally.

`CONFIG_RFKILL` is deliberately absent. `BT` depends on `RFKILL || !RFKILL`,
which is always satisfiable, and `hci_register_dev`'s rfkill calls are all
inside `#ifdef`. Without it there is simply no `/sys/class/rfkill` entry and
nothing to soft-block. Add it if a hardware radio switch is ever wired up.

---

## Firmware

`neodct/overlay/lib/firmware/rtl_bt/` carries exactly two files:

```
rtl8761bu_fw.bin        44,484 bytes
rtl8761bu_config.bin         6 bytes
```

plus `LICENCE.rtlwifi_firmware.txt`, which the licence requires to be
reproduced alongside a binary redistribution.

They are the upstream `linux-firmware` files, unmodified, byte for byte --
`sha256sum` them against the release named in `README` in that directory.

**Why not `BR2_PACKAGE_LINUX_FIRMWARE` + `..._RTL_87XX_BT`?** That is the
tidier route and it was the first choice. It is not taken because the
`linux-firmware` tarball is 583 MB compressed and Buildroot extracts the
*whole* tree into each output directory -- and there are two of them here,
`buildroot/output` and `build-luckfox`. On a machine with 17 GB free that is
a real risk of filling the disk mid-build, and it would also put 338 KB of
firmware for seven other Realtek parts onto a 128 MB NAND. Two files and a
licence is 44 KB and is exactly what this dongle asks for by name.

If the disk situation changes, swapping to the Buildroot package is a
one-line defconfig edit and deleting the overlay directory. The package
provides the same two filenames in the same place.

---

## Running it in QEMU

### With the real dongle

```sh
NEODCT_BT=1 neodct/tools/run_qemu.sh
```

`run_qemu.sh` adds `-device usb-host,bus=xhci.0,vendorid=0x2357,productid=0x0604`
(overridable with `NEODCT_BT_VENDOR` / `NEODCT_BT_PRODUCT`; any btusb-class
dongle works, since btusb matches on class rather than id).

**This needs permission on the dongle's USB node and does not have it by
default.** The nodes are `crw-rw-r-- root root`, QEMU has to open one
read-write, and it fails with `libusb: bad access (-3)`. The guest then sees
no USB device at all -- no error reaches the phone, so the symptom is a
Bluetooth app that says "no controller" for a reason nothing on screen can
explain.

The durable fix is a udev rule. **Paste this once, as the machine's owner:**

```sh
sudo tee /etc/udev/rules.d/70-neodct-bt.rules >/dev/null <<'EOF'
# TP-Link UB500 (RTL8761BU) -- readable by plugdev so QEMU can pass it
# through to the NeoDCT guest without running QEMU as root.
SUBSYSTEM=="usb", ATTR{idVendor}=="2357", ATTR{idProduct}=="0604", MODE="0660", GROUP="plugdev"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger --attr-match=idVendor=2357
```

The account already belongs to `plugdev`, so no re-login is needed. Check it
took with:

```sh
ls -l /dev/bus/usb/$(lsusb -d 2357:0604 | awk '{printf "%s/%s", $2, substr($4,1,3)}')
```

That must show `crw-rw----` and group `plugdev`. Unplug and replug the dongle
if it still shows `root root` -- the rule applies on the next add event.

Note that passing the dongle through **takes it away from the host** for the
lifetime of the QEMU process. `hci0` disappears from the host until the guest
exits.

### Without the dongle, or without that permission

QEMU's own emulated Bluetooth controller is not an option: the whole
`-bt`/`usb-bt-dongle` stack was removed in QEMU 6.0, and this host runs
11.0.3.

`CONFIG_BT_HCIVHCI` is the way in instead. Holding `/dev/vhci` open for one
second is enough to make the kernel register a virtual controller (the driver
schedules `vhci_open_timeout` at open and creates the device from it), so
from the phone's serial console:

```sh
cat /dev/vhci &          # hold it open
sleep 2
ls /sys/class/bluetooth  # hci0
```

That gives a controller the Bluetooth app can enumerate, which exercises the
whole `AF_BLUETOOTH` ioctl path on the target architecture and libc. It will
**not** come up: nothing is answering the controller-init commands the kernel
writes to `/dev/vhci`, so `HCIDEVUP` times out and the app's ADDRESS step
correctly reports `00:00:00:00:00:00`. That is the right answer for a
controller with no firmware behind it, and it is what the ADDRESS step is
for -- but it is not a working radio, and it should not be reported as one.

---

## The engineering app

**Bluetooth**, id 9007, engineering menu. Three pages off a list:

| Page | What it is for |
| --- | --- |
| Adapter | `hci0`'s registers at 1 Hz: name and bus, BD_ADDR, flag words, page/inquiry scan state, the ACL buffer the controller advertised, byte counters. The softkey toggles the radio. |
| Scan | A 5.1-second general inquiry, then the addresses and device classes that answered. |
| Self test | The five things that must be true, in dependency order. |

The self test is the point of the app. Bringing a dongle up fails in five
distinct places and four of them look identical from outside:

| Step | Passes when | A failure means |
| --- | --- | --- |
| KERNEL | `socket(AF_BLUETOOTH)` succeeds | `CONFIG_BT` is off. Needs a new kernel, and on this device that means a full reflash -- an `.ndsw` carries no kernel. |
| ADAPTER | `HCIGETDEVLIST` is non-empty | `btusb` did not bind: `CONFIG_BT_HCIBTUSB` off, or nothing plugged in. |
| ADDRESS | BD_ADDR is not all zeros | The firmware did not load. This is the one that looks most like working hardware. |
| RADIO | `HCIDEVUP` succeeds and `HCI_UP` is set afterwards | The controller is present and does not answer. |
| SCAN | `HCIINQUIRY` returns without error | The radio did not transmit. Hearing *nobody* is still a pass. |

Each step runs only if the one before it passed; the rest show `--` rather
than `FAIL`. Five failures caused by one unplugged dongle say less than one
failure and four dashes. The reason for the first failure is on the bottom
line, as `strerror(errno)`; when nothing failed, that line carries the
adapter's address instead.

**Scan and Self test block for about five seconds** and there is nothing to
draw meanwhile: `HCIINQUIRY` does not return until the controller has finished
listening, with no poll and no partial result. Both pages commit a
"Scanning..." / "Testing..." frame *before* the ioctl for that reason. The
wait is interruptible -- the kernel waits on the `HCI_INQUIRY` bit with
`TASK_INTERRUPTIBLE`, and `nd-apprun` installs SIGTERM without `SA_RESTART` --
so an incoming call arrives as `EINTR` and `app_shutdown()` still runs on
time.

---

## Luckfox

**Not done, and not verified.** See `docs/HARDWARE_NOTES.md`, section "SDK
kernel: Bluetooth", for the exact defconfig fragment and for what has to be
checked before it can be believed.

The short version: the SDK tree that builds the Luckfox kernel
(`~/Documents/Projects/luckfox-pico`) is not on this machine, so nothing about
the 5.10 vendor kernel could be built or tested. The rootfs half is done --
the firmware ships in the shared overlay, which both defconfigs use -- but a
kernel without `CONFIG_BT` makes that moot.

There is also a hardware question nobody has answered: the Pico Mini B has one
USB port, and the SIM7600 modem is on it. A dongle and a modem at once needs a
hub, and 500 mA of dongle on a board powered over that same USB is worth
measuring before trusting.

---

## Testing

```sh
cd neodct/src && make test          # includes test_bt and test_bluetooth
cd neodct/src && make ASAN=1 test
```

`test_bt.c` talks to whatever controller the machine running it actually has.
It uses `/sys/class/bluetooth` as an independent oracle and asserts that
`nd_bt_list()` agrees with it -- so it is true on a build machine with no
dongle *and* on a desk with one, and it is not a skip on the machine that has
the hardware. What it finds, it then checks properly: the name is `hciN` for
the id reported, and the address is not all zeros.

`HCIDEVUP` needs `CAP_NET_ADMIN`, which `make test` does not have, so the
tests exercise the failure paths there and the self test's RADIO step is
expected to report `Operation not permitted` on a developer's desktop. On the
phone everything runs as root and it succeeds.

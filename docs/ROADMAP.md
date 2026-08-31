# NeoDCT roadmap

What is planned, and why. Ideas are recorded here when they come up rather
than when they are started, so the reason for a decision survives the gap
between the two.

Version numbers below the line are intentions, not promises. The rule
followed so far: a third-number bump is a release you can install, and a
second-number bump waits until the phone can do something it could not do
before at all.

## Now: 0.3.10a -- updates that install themselves

The phone can find updates but cannot install one. Two separate faults,
both found while cutting 0.3.9a.

1. **The applier stages onto a partition too small to hold an update.**
   The Update app writes the whole system image to
   `/NeoDCT/User/.ndsys/incoming.img` before rebooting, and on the Luckfox
   that path is the `userdata` partition: 8 MiB, against a 51 MiB image. It
   can never fit, on any card. It works in QEMU only because `userdata`
   there is built at 512 MiB, which is why it went unnoticed until the
   first real install.

   The fix is the one recovery already uses: stream the image out of the
   `.ndsw` on the card straight to the system partition, and never make a
   copy at all. See `recovery_install_package` in `ndsys-recovery.sh`.

2. **"Look online" never found anything.** Fixed in 0.3.9a, but 0.3.9a
   cannot be installed for reason 1, so the fix ships again here.

0.3.10a is installed over USB in maskrom mode. After it, updates should
not need a cable again.

## 0.3.11a -- recovery you can actually drive

Recovery draws its menu on the phone's screen and reads the keys from the
same terminal. Nothing in the initramfs reads the keypad, so on hardware
it shows a menu that cannot be answered. The only input today is a serial
console, and the bench unit's serial wires break off their pads regularly
because there is no strain relief.

So a phone that will not boot is currently a phone that needs a soldering
iron. That is the thing to fix.

The owner has an approach in mind; it is not written down yet. What has to
be true whatever the approach:

- The initramfs has no Python. Whatever reads the keypad is a C helper or
  a busybox shell talking to the i2c device, in the same shape as
  `neodct_displayd`, which already gives recovery its panel output.
- It has to work with the system partition unmountable, because that is
  when recovery matters.

## Remote Shell -- ssh and sftp instead of serial wires

Built, needs an icon and a first real connection. See
`docs/REMOTE_SHELL.md`.

The phone dials out to a relay and carries a way back, because CGNAT and
IPv6-only mobile data mean nothing can connect to it. sshd binds loopback
and nothing else; the way in is the tunnel the phone opened, or nothing.

This does not replace serial entirely and is not meant to: a phone that
will not boot cannot start the thing you would use to log in. That is what
0.3.11a is for. What it does replace is every other day, which is most of
them.

Idea came from LeapPad developer mode, which exposes telnet and ftpd over
USB ethernet. The USB-C port here is charging only, so it goes over the
internet instead.

## 0.4.0a -- what a second-number bump is waiting for

Not scheduled. Recorded so the bar stays in one place instead of being
re-argued each time.

- **Power states.** The phone runs at full tilt whether or not anyone is
  holding it. Sleep, wake, and something sensible on the way to a flat
  battery.
- **The ST7789 blank pin.** Not connected on the bench unit. Without it
  the panel cannot be switched off, only painted black, which is most of
  the power a screen costs. Power states are hard to take seriously until
  this wire exists.

Either of these could arrive earlier as a 0.3.x if it lands on its own.
Together they are what would make the jump to 0.4 mean something.

## Unscheduled, wanted

- **Know when it is charging.** The phone reads the fuel gauge but cannot
  tell a charge from a discharge, so it cannot show a charging indicator
  or stop worrying about a low battery that is already on the cable.
- **A dev kit.** A second unit on a Luckfox Pico Mini A, which boots from
  an SD card instead of NAND: same RV1103, same 64 MB, a generic 4x4
  keypad on a PCF8575, a larger ST7789, USB ethernet, breadboarded flat.
  It runs the shipping armv7/musl binaries on the shipping silicon, which
  nothing except the bench phone does, and it cannot be bricked -- a bad
  kernel or initramfs costs a card reader instead of a maskrom session.
  That is what would unblock the changes currently gated on "needs a full
  reflash". It tests nothing about NAND, UBI or the enclosure, so it is a
  third rung and not a replacement for either end. `docs/DEVKIT.md` has
  the reasoning, the five changes the OS would need, and the hardware
  questions to settle first.
- **A Clock app, with alarms.** There is a Clock in the app list already;
  alarms are the part that is missing, and they need something to run
  while the UI is not the thing in front. Overlaps with power states: an
  alarm that cannot wake the phone is not an alarm.

## Known faults not yet scheduled

- **Recovery mounts vfat only.** `recovery_mount_card` tries `mount -t
  vfat`, while the running system tries `vfat exfat ext4 ext3 ext2`. Large
  cards are usually exFAT from the factory, and recovery reports "No SD
  card found" for a card that is present and working. Being fixed in
  0.3.10a because it is two words in a file already being changed.
- **Updates carry no kernel or initramfs.** A `.ndsw` is the rootfs and
  nothing else, so any fix to the boot path or the applier needs a USB
  flash to take effect. Worth revisiting if the boot partition ever gets
  big enough to hold two of anything.

  The consequence is a rule, not just an inconvenience: **the applier in
  the field decides what a pending record may say.** A new rootfs can be
  installed by an old initramfs, and that initramfs will never be updated
  by the thing it is installing. 0.3.10a is the case in point -- its
  Update app writes `package=` where earlier records said `image=`, and an
  older applier handed one of those reads it as an incomplete record,
  discards the update and reports a failure. Nothing is bricked, but
  nothing installs either, which is why 0.3.10a has to be flashed.

  So a change to `pending.prop` is only safe if either every phone that
  might read it has already been flashed, or the old applier still
  understands the new record. Adding fields is safe; changing what an
  existing field means, or moving what a record points at, is not.

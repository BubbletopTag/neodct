# The parity harness, in one page

Two machines are supposed to be one target. The emulator is a Cortex-A7 at
64 MB with the same musl, the same hard-float NEON-VFPv4 and the same 32-bit
`time_t` as the phone — but they are still two machines, and every way in
which they differ is either something a named stage will close, something that
can never converge, or something nobody noticed.

This directory is where the third kind stops being possible.

```
nd-inventory                   describes a machine, byte-identically twice
parity_capture_qemu.sh         one capture, from a built image under QEMU
parity_capture_hw.sh           one capture, from a phone, over ssh or serial
parity_diff.py                 holds the two against allow.txt
allow.txt                      every allowed difference, with its argument
qemu-armv7-probe.inventory     the committed emulator-side capture
test_parity_allowlist.py       the gate that needs no boot, in the pytest suite
../../tools/parity_capture_probe.sh  one capture, from a real armv7 boot, no image
../../tools/qemu_machine.sh    the device tree AND the kernel parameters,
                               shared with run_qemu.sh
../../tools/test_qemu_surfaces.sh  the gate that needs a boot and no image
```

## What is measured and what is not

**Nothing has ever been captured from a Luckfox Pico Mini B.** Nobody working
on this branch has one. `luckfox-armv7.inventory` does not exist,
`test_parity_allowlist.py` runs single-sided and prints *0 of N records
verified against hardware* on every run, and every `hw` value in `allow.txt`
carries `hw_evidence: unmeasured-claim`. Treat the phone column as a
prediction until somebody runs `parity_capture_hw.sh` and changes those fields
one at a time.

**The emulator side is measured, and not from an image.**
`qemu-armv7-probe.inventory` came from `parity_capture_probe.sh`: the repo's
own kernel (`board/qemu/armv7-virt/linux.config`), the exact machine
`run_qemu.sh` assembles — `-M virt -cpu cortex-a7 -smp 1 -m 64`,
`-global virtio-mmio.force-legacy=false`, `mtdram.total_size=0` plus the Pico
Mini's nandsim ID bytes and `nandsim.parts=2,2,4,128,64`, `video=vfb:on`,
`gpio-mockup.gpio_mockup_ranges=0,64`
and **the device tree `run_qemu.sh` builds** — and a **busybox initramfs**
where the NeoDCT rootfs would be, because `buildroot/output` does not exist
and a full build is hours. Two boots produced byte-identical files.

The device tree is not a detail. Three records here turn on the difference
between a subsystem being compiled in and a *device* being present, and a
capture taken without `-dtb` would describe a machine nobody boots. So
`parity_capture_probe.sh` **refuses** to run when it cannot build one, where
`run_qemu.sh` only warns: a session with no backlight is still worth having, a
baseline with no backlight is not. Both call `nd_dtb_build()` in
`neodct/tools/qemu_machine.sh` — which now also holds `nd_qemu_append()`, the
kernel parameters that decide which devices exist, so the three recipes cannot
drift apart either. They had: the surfaces test's hand-copied command line was
already missing the nandsim ID bytes while its comment called them
"run_qemu.sh's parameters, copied rather than invented".

So everything *below* the image is a real measurement: the kernel identity,
MemTotal, the class trees, the MTD geometry, `/proc/devices`,
`/proc/filesystems`, the input devices, the `/dev` families, the framebuffer
ioctls. Everything *about* the image is not: `os-release`, `/NeoDCT/platform`,
the users table, the mount set, and the six records that need libneodct all
read `ABSENT` or `UNAVAILABLE(nolib)` and are properties of that initramfs.
Those nine records carry the verdict `until-image` and must be revisited the
first time `parity_capture_qemu.sh` runs on a built image; the count is pinned
in the host test, and so is the total, so appending a record is a diff on an
integer somebody sees.

Three consequences worth knowing before the file confuses you:

- `fb0.var.xres` is **240** and `bits_per_pixel` is **32**, because the
  capture is taken **after** `neodct_displayd`'s `force_mode()` has run. There
  is no `S90display` in a busybox initramfs, so `parity_capture_probe.sh`
  cross-compiles the daemon in beside `nd-inventory` and runs
  `neodct_displayd --panel null --once` itself — the phone's binary, the
  phone's `FBIOPUT_VSCREENINFO`, on vfb, which is the driver both machines
  have. Ten `fb0` records moved when that landed and nothing else did.
  `NEODCT_REQUIRE_PANEL` is no longer a hook held open for a day that had not
  come: the check is the default in both capture scripts and
  `NEODCT_REQUIRE_PANEL=0` is the escape hatch. **`--panel null` and not
  `--panel stream:`** — the stream needs a virtio-console port, the port gives
  the guest `/dev/vport0p1` and `/sys/class/virtio-ports`, and `dev.count`
  would move. A baseline describes the machine somebody boots.
- `class.gpio`'s `gpiochip0` is `CONFIG_GPIO_MOCKUP`, not a real controller.
  It is there because `-M virt`'s own pl061 is eight lines at base 512, so
  none of the pin numbers this tree hard-codes — `ND_BL_GPIO_PIN` 53, and 56
  and 57 for the panel's RST and DC — exists on it. `gpio-sim`, which 6.12
  recommends instead, has no fixed gpiobase and cannot produce them.
- `class.mtd` is `[mtd0 mtd0ro ... mtd5 mtd5ro]` and **that is the phone's own
  key set**, because `nandsim.parts=2,2,4,128,64` in `nd_qemu_append()` cuts
  the simulated chip into `docs/PARTITIONS.md`'s six partitions at the phone's
  own mtd numbers. It used to be `[mtd0 mtd0ro]` with an allow.txt record
  explaining the difference, and that record is gone. What it cost is five
  `mtd.byname.[*].*` records plus `block.mtdblock5.size`: those geometry keys
  are named after the PARTITION, nandsim names its partitions after itself,
  and six partitions means thirty such keys where there were five — none of
  which was listed at all before, i.e. all thirty silently *required to agree*
  with a phone they cannot agree with. The phone's own thirty are the mirror
  image and are deliberately not written, because they do not exist until
  somebody captures a phone.
- every `dev.*` owner reads `uid=0(?)`, because that initramfs has no
  `/etc/passwd` to resolve against. The names are read from `/etc/passwd` and
  `/etc/group` directly and not through `getpwuid(3)` — see the record for
  `dev.[*]` in `allow.txt` for why that correction had to be made.

## The loop

```sh
# the emulator, with no image at all: the repo's kernel, a busybox rootfs, and
# a byte-for-byte comparison against the committed baseline. This is the gate
# that runs today, and `make parity-probe` is the same thing.
neodct/tools/parity_capture_probe.sh \
    --kernel /path/to/zImage --rootfs /path/to/busybox-rootfs \
    --compare neodct/tests/parity/qemu-armv7-probe.inventory

# the emulator, from a built image. qemu-armv7.inventory does not exist yet --
# the day it does, it becomes the baseline here and every `until-image` record
# in allow.txt is re-argued against it.
neodct/tools/parity_capture_qemu.sh --out /tmp/qemu.inventory \
    --compare neodct/tests/parity/qemu-armv7-probe.inventory

# the phone
neodct/tools/parity_capture_hw.sh --ssh neodct \
    --out neodct/tests/parity/luckfox-armv7.inventory

# the comparison
neodct/tools/parity_diff.py --qemu /tmp/qemu.inventory \
    --hw neodct/tests/parity/luckfox-armv7.inventory \
    --allow neodct/tests/parity/allow.txt

# skeletons for anything unexplained, with an empty `why` you have to fill in
neodct/tools/parity_diff.py ... --propose >> neodct/tests/parity/allow.txt
```

## What gates what

| gate | when it runs | what it catches |
|---|---|---|
| `test_parity_allowlist.py` | every change, no boot | an empty argument, an unedited `--propose` skeleton, a `permanent`/`until-image`/total count that moved, a column that matches every value, a `[*]` key free on both sides, an emulator column that no longer matches the committed capture, a baseline whose hashes do not verify or has none, a closed stage still promising to close, a `qemu-armv7.inventory` that has arrived unnoticed |
| `test_qemu_surfaces.sh` | when a kernel is in hand, no image | the small hardware surfaces in a booted guest — the backlight's name and table, the cpufreq table, the two deliberately empty classes and the absent one, the phone's three GPIO pins, **the flash: six MTD partitions at the phone's numbers, mtd4 at 8 MiB, writesize 2048** — MemTotal on both sides of `-dtb`, and the one thing no host test can see: a write in the wrong order being swallowed |
| `test_inventory` (`make test`) | every change, no boot | the canonicaliser: LC_ALL=C sorting, the `/dev` family collapse with the *first* member escaping it (which is what tests the majority rule — breaking a middle member cannot), every mask, `ABSENT` versus `[]`, key escaping, a duplicate key being refused, both line shapes of `/proc/filesystems`, and one tree built in two creation orders producing byte-identical output **and** the exact output a correct sort produces |
| `parity_capture_probe.sh --compare` (`make parity-probe`) | when a kernel and a busybox rootfs are in hand, no image | **emulator drift** — a fresh capture from a real boot must equal the committed baseline byte for byte, *and* `neodct_displayd`'s start-up path, which the capture now runs before `nd-inventory`. ~4 s. This is the only gate anywhere that re-derives the allowlist's input from a *machine* rather than checking it against a file |
| `test_qemu_nand.py` (`pytest`) | every change, no boot | the emulator's NAND without booting one: the de-interleave `/NeoDCT/User`'s persistence rests on (including that a page nandsim never programmed comes back as 0xFF and not 0x00), `nandsim.parts=` and `mkqemuflash.py`'s table being the same six partitions, `mknand.sh`'s LEB arithmetic and its `USERDATA_MAX_LEB`, that the flashing drives cannot wear `NDSYS` or `NDUSER`, that the cache drive is the last virtio-blk on the line, that the flasher is not packed into the phone's initramfs, and that the overlay cpio is reproducible and really unpacks |
| `test_displayd_stream.py` + `test_displayd` (`make test`) | every change, no boot | the panel daemon's composing half, driven through `--fb-at` and `--panel stream:` — the ST7789 bring-up, the CASET/RASET window including the `+65` letterbox, the one-pixel dirty rect, the frame skip, the big-endian 565 pack, and `st7789_replay.py`'s decode of all of it |
| `parity_capture_qemu.sh --compare` | on a built image | the same, from the image rather than from a busybox initramfs |
| `parity_capture_hw.sh` + `parity_diff.py` | when hardware is in hand | genuine cross-machine divergence |

The split is the point. Hardware is rare and kernel-config changes are not, so
the common case — somebody changes something and the emulator quietly drifts —
is caught with no phone in the room.

## The storage row, and why a virtio boot cannot be a baseline

`run_qemu.sh`'s default is `NEODCT_STORAGE=nand`: `/NeoDCT/User` is ubifs on
`ubi1:userdata`, over a simulated chip with the phone's geometry, flashed from
`mknand.sh`'s own `userdata.ubi`. A capture from that boot carries `class.ubi`
= `[ubi1 ubi1_0 version]`, the `ubi.*` geometry records, a `dev.ubi*` family
and a `/NeoDCT/User` mount whose fstype is `ubifs` — every one of which is a
record the phone will produce too.

A capture from `NEODCT_STORAGE=virtio` carries none of them. It is visibly a
different machine, so `parity_capture_qemu.sh --compare` fails on it byte for
byte with no new machinery — the same mechanism that already refuses a
shell-fallback capture as a gating reference. `parity_capture_qemu.sh` now
also refuses such a boot **before** it starts, because failing at the diff
means four minutes of booting and then a wall of differences whose cause has
scrolled off.

What the emulator still cannot do is put the SYSTEM volume on that chip at the
phone's memory, and both walls are measured rather than argued: without a
host-backed cache file a 51 MB `system.ubi` is 54,953 kB of unreclaimable
nandsim slab on a 53,824 kB machine, and with one the first read of the volume
that misses the cache file's page cache deadlocks the guest.
`NEODCT_STORAGE=nand-full` is that stack at `NEODCT_MEM=128` or more, and it
says in its own refusal that it is the phone's storage and not the phone's
memory. `docs/PARTITIONS.md` section 11 has the numbers and the stack trace.

**The committed baseline does not contain any of that, and the gate table
above must not be read as saying it does.** `qemu-armv7-probe.inventory` comes
from a boot with two blank virtio drives and no flasher, so it carries
`class.ubi [version]`, `block.vda.serial NDUSER`, no `ubi.*` geometry and no
`/NeoDCT/User` mount at all. **No gate anywhere re-derives a UBI record from a
machine**: change `nandsim.parts=`, `ndflash`'s VID header offset,
`mknand.sh`'s `LEB_SIZE` or `nd_ubiattach`'s ioctl struct and `make test`,
`make ASAN=1 test`, the pytest suite, `test_qemu_surfaces.sh` and
`make parity-probe` all stay green. That is deliberate — the probe has to
produce a byte-identical capture on any host with a kernel and a busybox
rootfs, and flashing a real volume would make `mtd-utils` a precondition of
the cheapest gate in the tree — but it is a hole, it is this size, and
`parity_capture_probe.sh`'s header carries the same paragraph. What covers the
storage stack today is `test_qemu_nand.py` (tables and arithmetic, no boot)
and a manual `run_qemu.sh`, where `ndflash` refuses out loud at every step.

**And that row is why the probe capture is a gate and not just an artefact.**
For as long as nothing re-derived the baseline, every host test validated
`allow.txt` against a *file*: `buildroot/board/qemu/armv7-virt/linux.config` and
`run_qemu.sh` — the two things that decide what the capture says — were read by
nothing. Measured: drop `gpio-mockup.gpio_mockup_ranges=0,64` from
`run_qemu.sh` and the emulator loses gpio53, 56 and 57 (`nd_backlight.c`'s GPIO
tier, and the panel's RST and DC), three capture records change, and every
gate stays green. A change to either file means running `make parity-probe`;
both files say so in their own headers.

## Which way this file has actually moved

**It grew, and saying otherwise was the most misleading sentence in it.** The
storage and panel stages took it from 21 records to 27, and by the number that
matters more — how many of the capture's keys are excused from parity — from
67 of 169 to 118 of 230. The fraction of a capture this file exempts went from
40% to 51%. Both integers are pinned in `test_parity_allowlist.py`
(`TOTAL_RECORDS_EXPECTED`, `PERMANENT_RECORDS_EXPECTED`,
`COVERED_KEYS_EXPECTED`) precisely so that neither can move without somebody
typing the new one.

The honest version of the good news is a different sentence, and it is not
"the file shrank":

- **Ten `fb0` records were not needed rather than being written.** Before the
  panel stage, a first hardware capture would have diverged on `xres`, `yres`,
  both virtuals, `bits_per_pixel`, `line_length`, `visual` and the three
  channel offsets — eleven records' worth of argument. `neodct_displayd` now
  runs on both machines and `force_mode()` sets all of them, so ten of the
  eleven agree by construction and only `fb0.fix.smem_len`, which no ioctl can
  reach, needed an entry.
- **Thirty `mtd.byname` keys stopped being silently required to agree.** They
  were unlisted, which in this harness means "must match" — and they could
  never match, because they are keyed by a partition name nandsim does not let
  anybody set. Six records now say so. That is the file getting *bigger* and
  the harness getting *more honest* at the same time, which is a trade worth
  making and worth naming.
- **One record closed outright**: `class.mtd`, deleted because the emulator's
  MTD listing became the phone's.

A record count going up is not by itself a failure; a record count going up
while the README says it went down is.

## The four verdicts

- **`until-stage-N`** — a divergence a named stage will close. This is what
  makes the file *shrink*, and it has happened once already. **Five** records
  said `until-stage-5`: `cpufreq.cpu0` (ABSENT here), `cpufreq.policies` and
  `class.backlight` (empty here), and `class.power_supply` and `class.thermal`
  (empty here too). The stage landed — a device tree for two of them, a kernel
  symbol *removed* for the other two — and:

  - `cpufreq.cpu0` and `cpufreq.policies` were **deleted**. The emulator now
    reports `present` and `[policy0]`, the same as the phone, and a record
    with nothing left to annotate is noise.
  - `class.backlight` was rewritten to `permanent`. `[backlight]` with
    `max_brightness 10` here, and on the phone either that or `[]` — the DTB
    lives in the boot partition and an `.ndsw` never replaces it, so both
    populations are in the field. That is a fact about the *phone*, not a
    stage anybody can close.
  - `class.power_supply` and `class.thermal` were rewritten to `permanent`
    too, in the other direction: `CONFIG_TEST_POWER` came out, so they are
    empty **by decision** rather than for want of a device model.

  Zero `until-stage-5` records remain, which
  `test_the_surfaces_stage_closed_its_records` asserts; leaving one in would
  also have failed `test_the_qemu_column_matches_the_committed_capture`,
  because the emulator column stopped matching the capture.
- **`until-image`** — the emulator side of this record is a property of the
  probe initramfs rather than of the emulator. Settled by the first capture
  from a built image, not by any stage.
- **`permanent`** — with an argument. `uname.release` is the honest example: a
  kernel cannot ship in a `.ndsw`, so the phone stays on the SDK's 5.10 until
  somebody reflashes it over a cable. The count is pinned in the host test, so
  adding one is a visible diff on an integer — and it has just gone from 5 to
  10 in one change, which is the largest move it should ever make without an
  argument attached. The five new ones are the small hardware surfaces:
  `power_supply` and `thermal` are empty *by decision* (`CONFIG_TEST_POWER`
  came out, and it was the only source of both), `leds` is absent by decision,
  `gpio` is a mockup chip that can never match the RV1103's controllers, and
  `backlight` is the rewritten stage-5 record above.
- **`must-differ`** — *required* to differ. `platform.record.image` is D1's
  discriminator; if the two sides ever agree, a QEMU-built `.ndsw` becomes
  installable on a phone in somebody's pocket. `parity_diff.py`'s
  `check_must_differ()` evaluates it against the two **captures**, not against
  the diff: a diff holds only keys that *differ*, so while it was evaluated
  there, a `must-differ` record whose two sides had come to agree matched
  nothing, was reported as `STALE`, and the operator was told to delete the
  guard.

## The rule about columns

Neither the `qemu` nor the `hw` column may be `~.*` or `~.+`. Eight records
carried one, six of them `permanent`, and those records could only ever say
*the emulator is what it was* — never *the two machines still agree*.
`class.gpio` was the sharpest: its own argument says it exists to pin `export`
and `unexport`, which is the whole legacy sysfs GPIO interface and one
`olddefconfig` from gone, and its `hw ~.*` pinned neither. Where the phone's
exact value cannot be predicted from this repository, its **shape** usually
can — a listing rather than `ABSENT`, a mount record rather than a missing one
— and that is what each `hw` column now says. A bare `~` is refused outright:
it used to be a total wildcard in one code path and the empty regex in every
other.

## The line against nd-selftest

**`nd-selftest` asks the kernel to DECIDE. `nd-inventory` asks the machine to
DESCRIBE.** One forks, drops privilege, performs an operation and reports
through an exit status; the other never forks, never changes euid, and every
line it prints is a fact. The rule that keeps that sharp as both files grow,
and it is in both headers: **`nd-inventory` may not contain the word FAIL, and
`nd-selftest` may not print a record.**

Both read `/proc/mounts`, and that overlap is deliberate, because the two fail
differently. `nd-selftest` fails on *this phone tonight* ("your user partition
is missing nosuid"); `nd-inventory` fails in a *pull request next week* ("the
phone and the emulator no longer mount `/NeoDCT/User` the same way"). A
divergence neither of them is wrong about is exactly the thing nobody was
catching.

## What is deliberately not in a capture

Nothing under `/NeoDCT/User` — it is the only writable partition and it holds
the phonebook, the SMS databases and Remote Shell's private key. No secrets or
per-unit identifiers: `serial_number` reaches the file as an attribute *name*
and never as a value, which is what makes names-not-values safe by
construction. No addresses. No clock and no sensor — not masked, *not read*.
No process list. No `/proc/config.gz`, because the two kernels are different by
design and 800 symbols would become 800 permanent records nobody reads.

And nothing that requires a write, a state-changing ioctl or a side-effecting
open. The rule is one line: *the inventory opens `/dev/fb0` `O_RDONLY` for two
GET ioctls and opens nothing else under `/dev`* — which is what makes it safe
to run on a phone somebody is holding.

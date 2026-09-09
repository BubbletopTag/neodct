# NeoDCT — working notes for agents

A Nokia-5190-style feature phone OS: Buildroot Linux, a Python UI drawn
straight to `/dev/fb0`, and a signed A/B-style update system. Target hardware
is a **Luckfox Pico Mini B** (RV1103 Cortex-A7, **64 MB RAM**, 128 MB SPI
NAND) with a 240×240 ST7789 panel and a SIM7600G-H modem.

The RAM budget is the constraint behind most of the architecture. Assume 64 MB
before suggesting anything.

## Layout

```
buildroot/          vendored Buildroot (tracked, ~15k files) — NOT a submodule
  configs/          the defconfigs actually used by the build
neodct/
  configs/          second copy of the defconfigs (see Gotchas)
  overlay/          everything that becomes the rootfs
    NeoDCT/System/  apps/ core/ ui/ hw/ — the Python OS
    bin/ etc/       run_neodct.sh, inittab, init.d
  initramfs/        boot-time update applier (busybox sh) + recovery
  scripts/          post-build / post-image hooks, mkinitramfs
  tools/            run_qemu.sh, sdcard.sh, mkupdate.py, uistub.py
  tests/            host-side pytest suite (never shipped)
docs/               hardware notes, bring-up logs, changelog
```

`netsurf-neodct/` and `build-*/` are untracked build scratch.

## Build and run

QEMU is the primary development path and the only target wired for the
current image design.

```sh
cd buildroot
make neodct_qemu_defconfig      # see the note below before skipping this
make                            # full image set
neodct/tools/run_qemu.sh        # from the repo root
```

**The emulator is the phone's part now.** `qemu-system-arm -M virt -cpu
cortex-a7 -smp 1 -m 64`, one ABI on both machines: same instruction set, same
musl, same hard-float NEON-VFPv4, same Thumb-2, same 32-bit `time_t`,
`size_t`, pointers and alignment. It was aarch64 on a Cortex-A53, and that
machine agreed with the Pico Mini about everything except the things that
break -- so those bugs passed here and failed on the bench. Measured on the
kernel this tree builds: `uname -m` = `armv7l`, CPU part 0xc07, MemTotal
54,808 kB of the 64 MB machine against the phone's ~54 MB -- 53,824 kB of it
the kernel's, and the last ~1 MB the device tree's. (It was 54,812 until the
keypad stage put a bus-number reservation node in that tree; the kernel
reserves `fdt_totalsize()`, so a bigger tree costs 4 kB. Measured on three
boots of one kernel that the i2c device itself costs nothing.) `run_qemu.sh` needs `dtc`
now: it appends `neodct/board/qemu/nd-virt-additions.dtsi` to the tree QEMU
generates, which is where the emulator's backlight and cpufreq policy come
from, and the kernel reserves `fdt_totalsize()` -- so passing a 8 KB tree
instead of the 1 MiB blob QEMU pads its own to gives ~1 MB BACK and MemTotal
rises. Without `dtc` the script says so and boots without any of them --
**three** things now and not two, because the keypad's bus NUMBER is a
device-tree fact as well: `virtio_mmio.c` never sets an `of_node`, so only the
reservation node puts the adapter at three. Measured, same kernel, `-dtb`
omitted: `/sys/class/i2c-dev` is `[i2c-0]`. A bus at the wrong number is worse
than no bus -- `nd_pcf8575_open(bus=3)` gets ENOENT and everything falls back
silently -- so `run_qemu.sh` takes the keypad away with the tree and says so.

The identity did NOT collapse with the ABI. The QEMU image is `qemu-armv7`
and the phone is `luckfox-armv7`; `nd_manifest_check_compatible()` still
compares that key byte for byte, so a package built here still refuses to
install on hardware. `qemu-aarch64` is retired: it survives as an accepted
alias for one ordered pair (an image saying `qemu-aarch64` takes an armv7
package, never the reverse) and as a name `mknap.py` and `nd_nap_install()`
refuse by.

**That alias does not by itself rescue an image flashed before the rename.**
The comparison runs in the libneodct of the image that is *running*, and an
older image is running the older code, which has no table. Reaching it takes
one transitional build --
`BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS="... qemu-aarch64"` on
`neodct_qemu_defconfig`, which `platform-id.sh` still maps, giving an armv7
image stamped with the old key that the old strcmp accepts. The argument, and
why the pair is ordered, is above `platform_alias_accepts()` in
`neodct/src/lib/nd_manifest.c`.

`.config` IS GENERATED FROM THE DEFCONFIG ONCE. It used to say "first time
only" here, and that was the bug: a tree whose `output/` predates a defconfig
change keeps its old `.config` through every rebuild and `make` never mentions
it. That is how images came to be built with no `ndusr` in them, so
`nd_priv_lookup()` found nothing and every app ran as root -- found with `top`
on a real build, not by anything in the tree. It is also how a tree keeps
building aarch64 after the change above: the giveaway is an `Image` in
`output/images/` where `run_qemu.sh` now wants a `zImage`.

The users specifically are now also declared in `package/neodct/neodct.mk`,
which reaches a stale `.config` because PACKAGES_USERS is collected whatever
the rootfs settings say. Nothing else is covered that way. Re-run the
defconfig after pulling anything that touches
`neodct/configs/neodct_qemu_defconfig`.

Build an installable update on an already-built tree (no rebuild):

```sh
NEODCT_SIGN_KEY=$PWD/../neodct/tools/devkey/neodct-dev.key make update
```

`run_qemu.sh` is driven entirely by environment variables — `NEODCT_STORAGE`,
`NEODCT_SNAPSHOT`, `NEODCT_VERITY`, `NEODCT_SD`, `NEODCT_RECOVERY`,
`NEODCT_KEYPAD`, `NEODCT_KEYS`, `NEODCT_MODEM`, `NEODCT_NET`, `NEODCT_DEBUG`
and more. Read its header before
adding a flag; the one you want probably exists.

Several of them **refuse** on the armv7 kernel and say what is missing:
`NEODCT_NET`, `NEODCT_MODEM`, `NEODCT_BT`, `NEODCT_AUDIO` and
`NEODCT_SD=share`. The kernel is the proven floor for memory parity and has
no PCI (so no xhci, so no USB at all), no `NETDEVICES`, no `VIRTIO_FS` and no
`VFAT_FS` — a card still attaches and nothing in the guest can mount it. The
absences and what each costs are listed in the header of
`buildroot/board/qemu/armv7-virt/linux.config`; adding one back means booting
the kernel again and writing down the new MemTotal.

**The emulator's `/dev/fb0` is the phone's, and the phone's own code put it
there.** It is vfb on both machines — the phone's own driver rather than a DRM
device pretending to be one — and it comes up 640×480 at 8 bpp on both.
`neodct_displayd`'s `force_mode()` is what makes it 240×175×32, and that
daemon now runs under QEMU as well: `S90display` starts it with `--panel null
--once` there and `--panel spidev` on hardware, the same binary and the same
`FBIOPUT_VSCREENINFO`. Measured on a real armv7 boot: `640,480 / 8 / 640`
before and `240,175 / 32 / 960` after, `red.offset` 0 — the phone's
framebuffer byte for byte, and `force_mode()` a tested path for the first
time. `neodct/tests/parity/qemu-armv7-probe.inventory` is captured **after**
it runs, so ten `fb0` records that a first hardware capture would have
diverged on now agree.

**The picture is rendered on the host, and always will be.** vfb has no
scanout: nothing a QEMU display frontend shows is the phone, whatever the mode
is. So the panel comes out as a byte stream instead — `neodct_displayd
--panel stream:<path>` writes every ST7789 command and every pixel the panel
would have received, `NEODCT_PANEL_STREAM=/tmp/panel.nd79 run_qemu.sh`
attaches the virtio-console port that carries it, and
`neodct/tools/st7789_replay.py --out a.png` decodes it into the composed
240×240 frame, letterbox and all. The format is pinned in
`neodct/src/displayd/nd_panel.h` and the decoder is deliberately a second
implementation written from the datasheet, so it can disagree with a wrong
encoder. The QEMU window is where a QWERTY keystroke comes from, and the
phone's OWN keys come from the keypad fifo instead -- see the next paragraph.
With no display at all the way into the evdev path is `NEODCT_MONITOR` and the
monitor's `sendkey`; the fifo needs no display at all.

**The keypad is a real i2c bus now, and it is a host process.** The phone's
keypad is a PCF8575 on `/dev/i2c-3`; `-M virt` has no i2c controller of any
kind, so until this stage `/sys/class/i2c-dev` was empty and `nd_pcf8575.c`,
`nd_matrix.c`, `nd_keypadsetup.c`'s 1,202 lines, the T9 surround and
`apps/KeypadMapperI2C` had run on exactly one machine in the world. The bus is
a QEMU `vhost-user-i2c-device` whose transfers are serviced OUTSIDE QEMU by
`neodct/tools/nd-i2c-keypadd`, a host program that models the expander at 0x20
and the MAX17048 fuel gauge at 0x36 from their datasheets. Keys go in through
a fifo, on the side the picture already comes out of:

```sh
echo 'tap num_5'   > ${TMPDIR:-/tmp}/neodct-keys   # press and release
echo 'press num_2' > ${TMPDIR:-/tmp}/neodct-keys   # hold
echo 'release all' > ${TMPDIR:-/tmp}/neodct-keys
echo 'short 9 14'  > ${TMPDIR:-/tmp}/neodct-keys   # a pin pair no key joins
```

`run_qemu.sh` starts the daemon before QEMU and reaps it on the EXIT trap,
because QEMU **refuses to start** when the vhost-user socket is not there
(measured: `Failed to connect to '...': No such file or directory`, exit 1).
That refusal is why the daemon goes first; it is **not** a guarantee that a
run either has a keypad or does not boot. `run_qemu.sh` catches a daemon that
will not start, drops the bus from the QEMU line and boots on evdev, loudly --
which is what makes `NEODCT_KEYPAD=off` an escape hatch rather than a
courtesy. `test_qemu_i2c.sh` and `parity_capture_probe.sh` REFUSE instead of
degrading, because a gate and a baseline are not sessions.

**QEMU's refusal only covers t=0.** Once it has connected, `i2c-virtio` waits
in `wait_for_completion_interruptible()` with the bus lock held and no timeout
anywhere in that path, so a daemon that dies mid-session does not leave a
keypad that stopped working -- it leaves a guest frozen in
`nd_input_read_key()`, with `nd_battery`'s poll stuck behind the same lock.
Measured: `kill -9` the daemon at scan pass 5 and the guest never printed pass
6. `run_qemu.sh` now watches the daemon's pid and stops QEMU with a message
rather than letting that be silent.

`NEODCT_KEYPAD=off` takes the bus away and names, on that boot, every path it
is not exercising; any other spelling is refused rather than quietly booting
with the bus.

**Two sessions on one host need two fifos.** The socket and the daemon log are
derived from `$NEODCT_KEYS`, so `NEODCT_KEYS=/tmp/keys-b run_qemu.sh` is a
genuinely separate second emulator; without that they shared one socket and
one log and the two daemons split the keystrokes between them (measured, 9/3
of twelve). `nd-i2c-keypadd` now refuses a socket another daemon is already
serving instead of unlinking it, which is also what surfaces an orphan left by
a session whose terminal died.

**The virtio keyboard stays**, and that is not an oversight. `nd_input_open()`
opens the matrix FIRST and then opens an evdev device REGARDLESS, and polls
both when both are present, because "a developer with a USB keyboard plugged
into a real phone can still type" -- so a machine with both is the first one
anywhere on which nd_input's backend SELECTION runs. Every boot logs which one
it took (`Input backend selected:`), and `run_qemu.sh` says on every boot what
to look for.

Measured on a booted guest, with the repository's own `nd_pcf8575.c` and
`nd_matrix.c` cross-compiled and run against the node:

```
I2C-ADAPTER i2c-3 name=i2c_virtio at virtio bus 1
I2C-NODE /dev/i2c-3 mode=0600 uid=0 gid=0 major=89 minor=3
I2C-FUNCS 0x0eff0009 I2C_FUNC_I2C=yes
I2C-XFER write=0 read=0 value=0xFFFE stage= errno=0
[BATT] MAX1704x fuel gauge @ 0x36 on /dev/i2c-3 (VERSION=0x0012).
I2C-SCAN pass=1 PRESS row=1 col=1
```

The gate is `neodct/tools/test_qemu_i2c.sh --kernel <zImage> --rootfs <dir>`.

The `bus 1` in that first line is **not** the i2c bus number and will not be
the same everywhere: it is `i2c-virtio`'s `snprintf` of `vdev->index`, the
count of virtio devices QEMU made before the adapter. That gate's boot has two
(keyboard + i2c) and says 1; `test_qemu_surfaces.sh` has one and says 0; the
parity capture has four and says 3. `allow.txt`'s `class.i2c-dev.i2c-3.name`
record is a regex for exactly this reason -- and the committed parity baseline
pins it anyway, so `make parity-probe` is sensitive to that boot's device
list. The parity README says what to do about it.

**One thing the emulator cannot reproduce, and it must not be papered over.**
virtio-i2c's status byte is OK-or-ERR with no error code, so an address nobody
answers reaches the phone's code as a SHORT WRITE with `errno` 0 where a real
controller gives ENXIO -- and `nd_input_errno_is_transient(0)` is false by
design, so the emulator calls an absent expander PERMANENT where the phone
calls it TRANSIENT and self-heals. Do not teach `nd_pcf8575.c` to synthesise an
errno from a short count: on the phone `i2c_master_send()` never returns one.
`test_keypad.c` pins the divergence and `allow.txt` records it.

Version comes from one place: `VERSION_ID` in `neodct/overlay/etc/os-release`.
An update built without bumping it installs but shows no change on screen.

## Tests

**The suite runs here again (2026-09-06).** It was blocked on the owner's own
workstation from 2026-09-05, because a test had reached `poweroff(8)` and
switched the machine off twice. That was a stop-gap while the containment was
built; the containment described below now exists and has been exercised, so
the restriction is lifted and the tests are expected to be RUN rather than
merely built. A release that says "tests unrun" is not finished.

`make test` is still the only way to run them -- the guard in every binary
refuses a bare `build/*/test/test_x`, and that refusal is deliberate:

```sh
cd neodct/src && make test              # the whole suite, in the sandbox
make test-one T=test_modem              # one binary, the same way
make ASAN=1 test                        # before pushing
```

`make test` runs the binaries **inside a sandbox** (`test/harness/sandbox.sh`:
bubblewrap with no D-Bus, no network, a minimal `/dev`, everything but the
checkout read-only, a private `/tmp` under `build/`), puts fake `poweroff`,
`reboot`, `systemctl` and friends first on `$PATH`, and fails the run if a
test ever reaches one. Every test binary also carries
`test/harness/nd_testguard.c`, which disarms the real halt inside libneodct
and refuses to start outside the harness. The reason is on the record: on
2026-08-31 and 2026-09-04 a test reached `poweroff(8)` and switched off the
workstation running it. Through `make test` that cannot happen any more, and
`build/*/test/test_x` run by hand stops before `main()` with a message
pointing at `make test-one T=test_x`. `NEODCT_ALLOW_BARE=1` overrides that
refusal for someone who has read the guard and accepts the risk;
`NEODCT_TEST_SANDBOX=none` skips the container on a disposable VM or CI box.
`bwrap` is required (`pacman -S bubblewrap`); without it `make test` refuses
rather than falling back to a bare run.

```sh
python3 -m pytest neodct/tests/ -q      # from the repo root
```

2,104 passing and 14 skipped, ~110s — measured, on this checkout. (It said
"510 tests, ~20s" here for a long time, and that is the number agents
calibrated on — `spec-build-test.md` risk R-15 is about exactly this. It then
said 1,961 across two commits that added tests without touching it, which is
the same failure at a smaller scale: a 78-test gap is wide enough to hide a
whole file that has stopped importing.) They import the real overlay code —
`conftest.py` puts `neodct/overlay/NeoDCT` on `sys.path` so `System.ui...`
imports resolve exactly as they do on the device. Run them before and after
any overlay change; they are fast enough that there is no excuse not to.

`neodct/tools/uistub.py` drives the *real* UI headlessly, capturing frames as
PIL images instead of writing to the framebuffer. Docs screenshots come from
`shoot_docs.py` on top of it — genuine output, not mockups.

The C equivalent is `nd-shoot`, which renders all 48 reference screens in
about two seconds:

```sh
./build/default/bin/nd-shoot --out DIR
./build/default/bin/nd-shoot --out DIR --wallpaper Classroom.gif   # every group
./build/default/bin/nd-shoot --out DIR --anim 125                  # a sequence
python3 neodct/tools/goldenframe.py --compare neodct/tests/golden DIR
```

`--wallpaper` forces one into the six groups whose recipe deliberately has
none, which is the only way to review a change to the shared background;
`--anim N` writes N consecutive home frames into `DIR/anim`, which is the only
way to look at an animated wallpaper rather than guess.

T9 — multi-tap, predictive, the `#` mode cycle and the mode indicator in the
composer's top right — runs only on the i2c matrix keypad, because a QWERTY
dev keyboard takes a different input path and genuinely has no modes.

**That used to mean "invisible on QEMU", and this recipe was how to see it.
The emulator has a matrix now**, so on a default boot `nd_input` should select
it and T9 should follow with no flag at all -- **after the first-boot keypad
wizard, which now runs here.** `nd_kpsetup_gate_check()` returns
`ND_KPSETUP_GATE_PROBE` the moment `/dev/i2c-3` exists (the `is_hw` test is
only reached when the node is ABSENT), `nd_main.c` calls
`nd_kpsetup_maybe_run()` on every boot, and no `keymap.json` ships in the
overlay -- it lives on `/NeoDCT/User`, which the wizard writes. So a fresh
userdata partition stops at sixteen enrolment prompts where this machine used
to go QUIET, and the comment at `nd_main.c`'s call site ("on a phone with no
i2c bus (QEMU) it gates itself off and is silent") is now stale. Feed the pad
in `nd_kpsetup_targets[]` order to clear it:

```sh
for k in navikey clear up down num_1 num_2 num_3 num_4 \
         num_5 num_6 num_7 num_8 num_9 num_0 star hash; do
    echo "tap $k" > ${TMPDIR:-/tmp}/neodct-keys; sleep 1
done
```

`NEODCT_SNAPSHOT=1` and `NEODCT_STORAGE=nand-full` both discard
`/NeoDCT/User`, so under either the wizard runs on **every** boot.
`run_qemu.sh` says all of this on any boot that attaches the bus. That the
enrolment completes this way has NOT been booted -- the daemon's key names are
`nd_kpsetup_targets[]` read row-major and `test_qemu_keypadd.py` pins that, but
no image exists in this container -- so it is written here as what to expect.
This is the first time `nd_keypadsetup.c`'s 1,202 lines will have executed
anywhere, which is worth having rather than skipping. That is a claim about an image
this container has never built -- `buildroot/output` does not exist -- so it is
written here as what to expect and not as something measured: what IS measured
is that `/dev/i2c-3` exists, that the repository's own scanner reads a
keystroke off it, and that `nd_battery` goes LIVE on the same adapter. Check
the boot log's `Input backend selected:` line before believing either way.

`NEODCT_T9` therefore does not go away; its job reverses. `NEODCT_T9=0` becomes
the only way to reach the QWERTY/DEV_KEYMAP text path on a machine where the
matrix is the default -- and that path ships on the phone, for the USB-keyboard
case. `NEODCT_T9=1` is still what a `NEODCT_KEYPAD=off` boot needs. The boot
script sources `/NeoDCT/User/env.sh` if it exists, which is the way to set
either without rebuilding a read-only rootfs:

```sh
echo 'export NEODCT_T9=1' > /NeoDCT/User/env.sh
```

**`env.sh` is gated.** It is arbitrary shell run as root from writable storage
on every boot — `SECURITY-AUDIT.md` section 4 Q5 vector 2, and the reason a
phone somebody can write one file on stays backdoored across updates, since an
update replaces only the rootfs. It is now sourced only when something
*outside* the writable partition says so: `neodct.devenv=1` on the kernel
cmdline, or `/etc/neodct-devenv` in the read-only, verity-covered rootfs.
`run_qemu.sh` passes the cmdline flag by default, so the workflow above is
unchanged in QEMU; `NEODCT_DEVENV=0 run_qemu.sh` takes it away, which is how to
see what a shipped phone does with an `env.sh` left on the partition.
Engineering mode is deliberately not accepted as the gate: it lives in
`settings.prop`, on the partition the attacker just wrote to.

The C build has its own suite — `cd neodct/src && make test`, and
`make ASAN=1 test` before you push anything; both run in the sandbox
described at the top of this section. It includes the golden frames in
`neodct/tests/golden/`, captured from the Python build during the port.

**Neither suite can see the confinement.** Every security test in the tree
checks what the image was *built* to do; none can check what the kernel
*decides*, because a build host has no `ndusr`, no `/dev/i2c-3`, a writable
root and possibly no mount namespaces. (The symbol to grep a kernel config
for is `CONFIG_NAMESPACES`, and the thing to look at on a running system is
`/proc/self/ns/mnt`. `CONFIG_MNT_NS` does not exist -- there is no such
symbol anywhere in the kernel tree, mount namespaces are unconditional
wherever `NAMESPACES` is set, and the `CONFIG_MNT_NS=y` line in
`buildroot/board/qemu/aarch64-virt/linux.config` has been silently discarded
by `olddefconfig` for as long as it has been there.) That half is
`nd-selftest`, which ships
in `/NeoDCT/System/bin` and runs on the phone or in QEMU:

```sh
nd-selftest              # everything; exit 1 if anything failed
nd-selftest boundary     # can ndusr_ut reach the databases, the keys, the records
nd-selftest processes    # who is actually running as whom, right now
```

It forks and really drops before each probe, so the answers are the kernel's.
A SKIP is not a PASS — it means the check did not run, usually because the
device or the user is not there. Run it after anything that touches
`users-table.txt`, the udev rules, `S00userdata`, the mount options or
`nd_priv.c`.

**The other half of that is `nd-inventory`, and it asks the opposite
question.** `nd-selftest` asks the kernel to DECIDE; `nd-inventory` asks the
machine to DESCRIBE. It writes down what is on the machine it is standing on
-- kernel identity, MemTotal, the sysfs class trees, the MTD geometry, the
`/dev` node families, the framebuffer ioctls, the platform record and the
modem's cold verdict -- as one sorted, byte-stable record per line, so that a
capture from the phone and a capture from the emulator can be diffed in a pull
request. It ships in `/NeoDCT/System/bin` beside `nd-selftest`, never forks,
never changes euid, reads no clock and no sensor, and opens `/dev/fb0`
`O_RDONLY` for two GET ioctls and nothing else under `/dev`, which is what
makes it safe to run on a phone somebody is holding.

```sh
nd-inventory                      # the whole capture
nd-inventory --self-check         # collect twice and refuse if they differ
```

`--self-check` takes a section list like every other invocation, and the
capture scripts deliberately pass none: the `kernel` section is three strings
from one `uname(2)` call and cannot differ, so scoping the check to it proved
nothing about the readdir-driven collectors that are the whole reason the
check exists.

The allowlist of permitted differences, the committed emulator-side capture
and the loop that maintains them live in `neodct/tests/parity/` -- start with
its README. The gate that runs with no phone and no built image is
`neodct/tests/test_parity_allowlist.py` in the pytest suite and
`test_inventory` in the C suite. **Nothing has ever been captured from real
hardware**, so that suite runs single-sided and says so on every run; the
emulator-side capture is real but comes from a busybox initramfs on the repo's
own kernel rather than from a built image, and the file says which records
that makes untrustworthy.

The line between the two tools, which goes in both headers: **`nd-inventory`
may not contain the word FAIL, and `nd-selftest` may not print a record.**

**And one thing neither of them can do, which is ask the kernel to REFUSE a
write.** `neodct/tools/test_qemu_surfaces.sh --kernel <zImage> --rootfs <a
busybox directory>` boots the emulator twice and asserts the small hardware
surfaces: that `/sys/class/backlight` holds exactly one device named
`backlight` with `max_brightness` 10, that `/sys/class/power_supply` and
`/sys/class/thermal` are EMPTY and `/sys/class/leds` absent, that gpio53,
gpio56 and gpio57 export, and that a `scaling_min_freq` written before
`scaling_max_freq` while RAISING is silently swallowed -- which is what
`nd_cpufreq_max_first()` exists for and what `test_cpufreq.c` says in its own
header it cannot check, because two ordinary files hold both values whichever
order they were written in. Since the keypad stage it also asserts that there
is exactly **one** i2c adapter, that it is bus **three**, and that
`/dev/i2c-3` arrives from devtmpfs `root:root 0600` -- which is the state the
udev rule then has to change, and the window
`nd_kpsetup_open_keypad_as_root()` exists to step over. It also asserts
MemTotal on both sides of `-dtb`,
which is the only thing that checks that number **in a booted guest**. The
other half is `test_parity_allowlist.py`, which asserts it out of the
committed capture with no boot at all -- both are gates and neither is
redundant, because one measures the machine and the other pins what the
committed artefact says about it.

**And the drift gate is `make parity-probe`.** From `neodct/src`, with a
zImage and a busybox rootfs in hand:

```sh
make parity-probe ND_KERNEL=<zImage> ND_ROOTFS=<a busybox directory>
```

It boots the emulator, takes a fresh `nd-inventory` capture and requires it to
equal `neodct/tests/parity/qemu-armv7-probe.inventory` byte for byte. It costs
about four seconds and it is the only thing anywhere that re-derives the
parity baseline from a MACHINE rather than checking the allowlist against a
file. **A change to `buildroot/board/qemu/armv7-virt/linux.config` or to
`run_qemu.sh`'s machine means running it** -- those two files decide what the
capture says, and until this existed nothing read them: dropping
`gpio-mockup.gpio_mockup_ranges=0,64` cost the emulator gpio53, 56 and 57 --
the backlight's GPIO tier and the panel's RST and DC -- with every gate in the
tree still green.

One consequence worth knowing before it confuses you: **if you create an
`ndusr_ut` on your build host, `test_browser` starts exercising the real
privilege drop.** That is deliberate and it is the only way to cover
`apps/Browser`'s `nd_priv_lookup()` from a host at all, but it needs root —
as a normal user with that account present, those cases print SKIP.

**Golden frames are a regression net, not a gate.** The port is finished and
apps are now being deliberately redesigned, so a frame that stops matching
because you changed that screen on purpose is the point, not a failure — re-cut
it and say so in the commit. Their value now is telling you that changing screen
A did not disturb screens B through Z. Do not leave a screen alone because a
picture of it exists, do not ask permission to change one, and do not cut a new
frame for a new screen. See CODING-STANDARDS.md section 7.

## The image design (understand this before touching storage)

At runtime `/` and `/NeoDCT/System` are **read-only squashfs** under dm-verity.
`/NeoDCT/User` is the only writable storage — **ubifs on `ubi1:userdata` on the
phone and in QEMU, ext4 on a partition where there is a block device**. It
never appears in `/etc/fstab`: `neodct/initramfs/init` finds it (by disk
serial `NDUSER`, then `LABEL="NDUSER"`, then the `neodct.user=` cmdline hint),
mounts it, `mount --move`s it into the new root, then `switch_root`s.

**The emulator's `/NeoDCT/User` is now the phone's, and `run_qemu.sh` flashes
`mknand.sh`'s own image to get it there.** `NEODCT_STORAGE=nand` is the
default: nandsim at the Pico Mini's ID bytes, `nandsim.parts=2,2,4,128,64`
giving `docs/PARTITIONS.md`'s six partitions at the phone's mtd numbers, and a
QEMU-only flasher (`neodct/initramfs/qemu/ndflash`, packed in as a second cpio
archive with `rdinit=/ndflash`) that writes `userdata.ubi` onto mtd4 and
attaches UBI over it before `exec`ing the phone's `/init`. Measured on a real
boot: `LEB size: 126976 bytes`, `VID header offset: 2048`, volume `userdata`
dynamic at 40 LEBs, `mount -t ubifs ubi1:userdata`. `user_is_ubi()` and the
ubifs branch of the mount had never executed anywhere before.

**The system half is NOT on the chip, and that is measured rather than
forgotten.** A 51 MB `system.ubi` costs 54,953 kB of unreclaimable nandsim
slab, which OOM-panics a 64 MB guest; `nandsim.cache_file=` removes that cost
and deadlocks the guest instead, on the first read of the system volume that
misses the cache file's page cache, because servicing a `ubiblock` request
then submits a second bio from inside the first one's dispatch.
`NEODCT_STORAGE=nand-full` is the whole stack — `/dev/ubiblock0_0`, squashfs
and dm-verity over it — and it refuses below `NEODCT_MEM=128` and says why.
`NEODCT_STORAGE=virtio` is the old arrangement and prints on every boot what
it is not exercising. `docs/PARTITIONS.md` section 11 has all of it.

**The NAND does not persist across QEMU processes** — nandsim's
`pages_written` bitmap is per-boot — so `run_qemu.sh` lifts the userdata
partition out of the cache file when QEMU exits and hands it back next boot.
That lift is on an EXIT trap and not after the QEMU line, which is the
difference between a save that happens and one that happens on the happy path
only: under `set -e` a non-zero QEMU exit terminated the script before the
save, and a `kill` of the script orphaned QEMU and skipped it too. Both
measured, both fixed; `run_qemu.sh` now `exec`s only on the plain virtio boot,
which is the one mode with nothing to save and nothing to clean up.

**Three storage modes, three different fates for `/NeoDCT/User`**, and each
one says which on every boot: `nand` (the default) saves it,
`NEODCT_SNAPSHOT=1` does not because every drive is copy-on-write, and
`nand-full` does not because it deliberately has no cache file to lift out of.
`NEODCT_KEEP_USERDATA=1` belongs to the **ext4/virtio** path only — it keeps
`userdata.ext4` across a rebuild by rewriting `installed.prop` with `debugfs`,
and there is no host-side way to rewrite one file inside a ubifs volume, so
`post-image-neodct.sh` says so rather than reporting a success the NAND path
does not get.

An `UPDATE.ndsw` is a zip of `rootfs.squashfs` + `manifest.json` +
`manifest.sig` — **the entire root filesystem**, not the `/NeoDCT` directory.
Two consequences worth internalising:

- Anything on `/` is destroyed by an update. Only `/NeoDCT/User` survives.
- **There is no kernel in the package.** `manifest.py` only *checks*
  `min_kernel`. Kernel-config changes cannot ship over the air; they need a
  full reflash.

So persistent config belongs in `neodct/overlay/` (baked into the rootfs) or on
`/NeoDCT/User`. Writing to `/etc` at runtime is not a fix — it cannot work on a
real image.

`docs/TESTING_UPDATES.md` covers building updates and reproducing every refusal
path on purpose.

## Conventions

- **Framework elements never fill their own background.** A widget or an app
  screen that wants a blank background calls `nd_ui_paint_chrome_full()` or
  `nd_ui_paint_chrome_content()`, which paints the wallpaper or black
  depending on `system.ui.wpeverywhere` and the app's `manifest.json`
  `useWallpaper`. A literal `ND_BLACK` fill is right only for a surface that
  is not chrome — a game's play field, Koki's own canvas, the LCD test.
- Python: 4-space indent, `snake_case`, `PascalCase` classes, `UPPER_SNAKE`
  constants. Beyond the standard library the target has only what the defconfig
  builds — currently Pillow, mutagen, miniaudio, plus `sqlite3`/`ssl`/`lzma`.
  Anything else means adding a Buildroot package, not a `pip install`.
- **Comments explain why, not what.** This codebase is unusually good about
  documenting the reasoning behind a non-obvious choice — module docstrings
  that explain ordering guarantees, inline notes about why dmix or `.pyc`
  caching is handled a particular way. Match that. Do not add narration.
- Apps live at `overlay/NeoDCT/System/apps/<AppName>/` with `manifest.json`,
  `main.py`, `icon.png`.
- Absolute runtime paths (`/NeoDCT/System/...`) are load-bearing — `uistub.py`
  remaps them for host tests. Do not make them relative.
- Shell scripts target busybox ash, not bash. The initramfs parses records with
  `while IFS='=' read`, never by sourcing, so a value can never execute.
- Commits are short, imperative, often scope-prefixed (`configs: ...`,
  `browser: ...`).

## Gotchas

- **Two copies of every defconfig** — `buildroot/configs/` and
  `neodct/configs/`. The build uses the `buildroot/` copy. Edit both, or the
  next person builds something else. Verify with `diff` before committing.
- **The luckfox target is now on the immutable design** — squashfs root under
  dm-verity on `/dev/ubiblock0_0`, ubifs userdata on `ubi1`, initramfs built
  into the kernel. `docs/PARTITIONS.md` has the layout, `neodct/tools/mknand.sh`
  builds the images. It needed a repartition (oem dropped, boot grown to 16M)
  and a kernel rebuilt with `DM_VERITY`, so a phone on the old table must be
  fully reflashed, not updated. Kernel cmdline lives in the **U-Boot env**, not
  the DTS, which is ignored on this board.
- Out-of-tree build dirs bake in absolute paths. `buildroot/output` and
  `build-luckfox/` were both built at a `/home/bubbles/Documents/...` path that
  no longer exists; host tools carry it in shebangs and RPATHs, and `fakeroot`
  dies with "libfakeroot.so not found". Rebuilding just `host-fakeroot` was
  enough both times, but a cold rebuild is the only real cure.
- **Extra kernel args must not use an `RK_*` name** in the SDK board config.
  `unset_env_config_rk()` clears those with `env | grep -oh "^RK_.*=" | source`
  and that `.*` is greedy, so a value containing `=` becomes a malformed line
  that is sourced — the build dies on an unterminated quote before printing
  anything. Use `NEODCT_BOOTARGS_EXTRA`.
- SDK kernel builds must happen in an Ubuntu 22.04 distrobox; native Arch hangs
  on the atbm wifi driver. See `docs/HARDWARE_NOTES.md`.

## Hardware access

Serial console: `/dev/ttyUSB0`, 115200 8N1, root login with no password.
Flash the system partition only, leaving boot alone:

```sh
sudo ./upgrade_tool di -rootfs system.ubi   # luckfox-pico/tools/linux/Linux_Upgrade_Tool/
```

**`-rootfs` names the PARTITION, and the image that belongs in it is
`system.ubi`.** Not `rootfs.ubi` or `rootfs.ubifs`, which the build still
produces — the UBIFS/UBI block in the luckfox defconfig is the only thing
that pulls `host-mtd`, and `mknand.sh` needs its `ubinize` and `mkfs.ubifs`,
so the files exist, look right, and are the pre-verity layout. Flashing one
gives a UBI image with a `rootfs` ubifs volume where the cmdline expects a
static `system` volume, so `ubi.block=0,system` matches nothing, the
initramfs finds no system image and drops into recovery. Getting back needs
maskrom mode and a cable. `docs/HARDWARE_NOTES.md` and `docs/FLASHING.md`
have the full procedure.

The board has a working SD/MMC controller (`mmc1` binds on the running kernel,
and an `SD_CARD` board config exists for the Mini), so SD is a genuine option
for user storage — relevant given only 128 MB of NAND.

## Releases

Cutting a release is pushing a tag; `.github/workflows/release.yml` does the
rest. Use the helper rather than tagging by hand:

```sh
neodct/tools/release.sh --dry-run    # what it would tag, and the notes
neodct/tools/release.sh              # tag + push
```

The version is never typed in. It comes from `VERSION_ID` in
`neodct/overlay/etc/os-release` -- the same field the image reports -- and the
workflow **fails the release** if the tag disagrees with it. Release notes are
the matching section of `neodct/overlay/NeoDCT/CHANGELOG.txt`, so a version
with no changelog section is refused before it is tagged.

Tags carry no leading `v` (`0.3.7a`); the workflow accepts the older `v0.1.5a`
form too. Releases are marked pre-release, as every release so far has been.

**The workflow attaches no `.ndsw`, and that is not the same as a release
having none.** Building one means building the whole buildroot tree and needs
the signing key, and publishing an unsigned package would be worse than
publishing none -- the phone shows "BAD SIGNATURE! UPDATE MAY BE CORRUPT!!"
to anyone who installs it. So `release.sh` builds nothing either: it waits
for the workflow to create the release, then uploads whatever signed,
version-matching packages it finds in `buildroot/output/images` and
`build-luckfox/images`, one asset per platform. 0.3.13a and 0.3.14a each
carry two.

The asset name is `UPDATE-<platform>.ndsw` and the phone downloads by exactly
that name, with no fallback -- so releases cut before the emulator moved to
armv7 hold `UPDATE-qemu-aarch64.ndsw`, and a `qemu-armv7` image looking
online finds nothing until the first release after it. That is the design
working: a fallback would pull ~58 MB over a bearer that can take an hour
before anything got a chance to refuse it.

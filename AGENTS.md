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

`.config` IS GENERATED FROM THE DEFCONFIG ONCE. It used to say "first time
only" here, and that was the bug: a tree whose `output/` predates a defconfig
change keeps its old `.config` through every rebuild and `make` never mentions
it. That is how images came to be built with no `ndusr` in them, so
`nd_priv_lookup()` found nothing and every app ran as root -- found with `top`
on a real build, not by anything in the tree.

The users specifically are now also declared in `package/neodct/neodct.mk`,
which reaches a stale `.config` because PACKAGES_USERS is collected whatever
the rootfs settings say. Nothing else is covered that way. Re-run the
defconfig after pulling anything that touches
`neodct/configs/neodct_qemu_defconfig`.

Build an installable update on an already-built tree (no rebuild):

```sh
NEODCT_SIGN_KEY=$PWD/../neodct/tools/devkey/neodct-dev.key make update
```

`run_qemu.sh` is driven entirely by environment variables — `NEODCT_SNAPSHOT`,
`NEODCT_VERITY`, `NEODCT_SD`, `NEODCT_RECOVERY`, `NEODCT_MODEM`, `NEODCT_NET`,
`NEODCT_DEBUG` and more. Read its header before adding a flag; the one you want
probably exists.

Version comes from one place: `VERSION_ID` in `neodct/overlay/etc/os-release`.
An update built without bumping it installs but shows no change on screen.

## Tests

Run the C suite, freely and often, with `make test` -- and only that way:

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

510 tests, ~20s. They import the real overlay code — `conftest.py` puts
`neodct/overlay/NeoDCT` on `sys.path` so `System.ui...` imports resolve exactly
as they do on the device. Run them before and after any overlay change; they
are fast enough that there is no excuse not to.

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
dev keyboard takes a different input path and genuinely has no modes. So on
QEMU none of it is visible by default. `NEODCT_T9=1` overrides that; the boot
script sources `/NeoDCT/User/env.sh` if it exists, which is the way to set it
without rebuilding a read-only rootfs:

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
root and possibly no `CONFIG_MNT_NS`. That half is `nd-selftest`, which ships
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
`/NeoDCT/User` is a **separate ext4 partition** and the only writable storage.
It never appears in `/etc/fstab`: `neodct/initramfs/init` finds it (by disk
serial `NDUSER`, then `LABEL="NDUSER"`, then the `neodct.user=` cmdline hint),
mounts it, `mount --move`s it into the new root, then `switch_root`s.

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
Flash rootfs only, leaving boot alone:

```sh
sudo ./upgrade_tool di -rootfs rootfs.ubifs   # luckfox-pico/tools/linux/Linux_Upgrade_Tool/
```

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

**No `.ndsw` is attached yet.** Building one means building the whole
buildroot tree and needs the signing key, and publishing an unsigned package
would be worse than publishing none -- the phone shows "BAD SIGNATURE! UPDATE
MAY BE CORRUPT!!" to anyone who installs it. Attach one by hand for now.

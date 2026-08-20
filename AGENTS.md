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
make neodct_qemu_defconfig      # first time only; overwrites .config
make                            # full image set
neodct/tools/run_qemu.sh        # from the repo root
```

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

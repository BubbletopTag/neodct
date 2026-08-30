# Buildroot integration and the test oracle — C port specification

**Subsystem:** how the OS is *built* (Buildroot, defconfigs, post-build/post-image hooks)
and how the port is *proved correct* (the headless UI stub, the deterministic frame
capture, and the 659-function pytest suite).

**Surveyed at:** commit `c4a529f4`, Buildroot **2025.11**, `VERSION_ID=0.3.13a`.

**Status of what already exists:** `neodct/tools/goldenframe.py` and
`neodct/tests/golden/` (49 reference frames + `manifest.json`) were committed in
`c93e3117` / `21f9a556`, before this survey. They are a very good start and are
**not yet device-accurate** — section 3.9 explains exactly why and how to fix it. That
one issue is the single most important finding in this document.

---

## What this does (plain English)

### The build side

NeoDCT is not "an app you install on Linux". It *is* the Linux. Every boot, the phone
runs a filesystem that was assembled on a desktop PC by a tool called **Buildroot**.

Buildroot works like this:

1. You give it a **defconfig** — a shopping list. `neodct/configs/neodct_qemu_defconfig`
   is 85 lines and each line is one item: "include Python", "include a JPEG decoder",
   "include the SSH server". Buildroot downloads each item's source code, cross-compiles
   it for the phone's ARM chip, and drops the result into one directory tree
   (`buildroot/output/target/`) that looks exactly like the phone's future `/`.
2. It then copies the **overlay** (`neodct/overlay/`) on top. That is the actual NeoDCT
   operating system — all the Python, the fonts, the wallpapers, the app icons.
3. It runs the project's **post-build scripts**, which tidy up: delete apps that were
   removed from the overlay, delete host-built `.pyc` files, write `version.prop` so the
   phone knows what version it is, render the boot banner.
4. It squashes the whole tree into one compressed read-only file (`rootfs.squashfs`).
5. It runs the project's **post-image script**, which bolts a cryptographic checksum
   tree onto the squashfs (dm-verity), builds the initramfs, and makes an empty
   user-data partition and a blank SD card image.

The important thing to understand for the rewrite: **nothing in that pipeline knows or
cares that NeoDCT is written in Python.** Buildroot builds *packages*. Python happens to
be one of the packages on the shopping list, and the overlay happens to contain `.py`
files. Replacing Python with C means: remove some lines from the shopping list, add one
new line for a package that builds our own C code, and put `.so` and binary files in the
overlay where `.py` files used to be. The rest of the machinery is untouched.

There is a precedent already. The project ships **one C program today**:
`neodct_displayd`, the driver that pushes pixels to the physical screen over SPI. Its
source (`neodctDisplay.c`, 654 lines) sits in the overlay, and its **compiled binary is
committed to git alongside it** — somebody built it by hand on their machine and checked
in the result. That works for one small program. It will not work for a whole operating
system, so the port needs a real Buildroot package. Section 6 says exactly what files
that package consists of.

### The test side

There is a second, completely separate thing in this subsystem, and it is the reason the
rewrite is possible at all.

`neodct/tools/uistub.py` is a **fake phone**. It boots the real NeoDCT code — the real
`main.py`, the real menus, the real apps — on a desktop PC, with no screen, no keypad,
no modem, and no `/NeoDCT` directory. It does this by three tricks:

- `CapturingFramebuffer` pretends to be the screen. Instead of writing pixels to
  hardware, it keeps each finished picture in a list.
- `PathRemap` intercepts every file operation (`open`, `os.listdir`, `sqlite3.connect`,
  `PIL.Image.open`, …) and rewrites any path starting `/NeoDCT/` so it points at a
  throwaway copy of the overlay in `/tmp` instead.
- `KeyScript` pretends to be the keypad and plays back a canned list of button presses.

So you can say "boot the phone, press Down, press Enter, give me the picture" and get
back a real 240×175 image drawn by the real code. That is where the README screenshots
come from — they are genuine output, not mockups.

`neodct/tools/goldenframe.py` takes that one step further. The fake phone is *real* but
not *repeatable*: the clock in the status bar reads the actual time, the unread-mail
envelope blinks, the games seed their randomness from the clock. Run it twice, get two
different pictures. `goldenframe.py` freezes all of that — a fake clock that starts at a
fixed moment and ticks forward exactly one tenth of a second every time a picture is
drawn, the timezone pinned to UTC, the random-number generator seeded to a constant.
Now the same script always produces byte-for-byte the same 49 pictures.

**That is the oracle.** Capture 49 pictures from the Python build. Capture the same 49
from the C build. Compare the pixels. If they match, the port is correct — not "looks
right to me", *correct*, as a checksum. If they do not match, the tool tells you which
screen, how many pixels, and where on the screen the box of differences is.

It takes **5.7 seconds** to render all 49 frames. It can run on every commit.

There is a catch, and it is important. The 49 reference pictures that are committed
today were rendered on a developer's desktop, where the text-layout engine Pillow picks
up is a library called **Raqm**. The phone's Pillow is built with Raqm deliberately
switched off. Those two engines position letters differently. I measured it: **46 of the
49 pictures change**, by up to 16.8% of the screen. So the committed references are a
faithful picture of *the developer's laptop*, not of *the phone*. They have to be
recaptured with the layout engine pinned before they can be used to judge the C port.
That is a five-line change to `goldenframe.py` plus one recapture. Section 3.9.

---

## Files and where they go in C

### Buildroot

| Path | Size | Purpose | Fate in the C port |
| --- | --- | --- | --- |
| `buildroot/` | 15,026 tracked files, Buildroot **2025.11** | Vendored upstream Buildroot. **Tracked in git**, not a submodule. `buildroot/output/` and `buildroot/dl/` are gitignored. | Unchanged upstream. Only `configs/` and one new `package/neodct/` directory (or a br2-external tree) are ours. |
| `buildroot/package/` | 2,991 packages | Verified **byte-identical to upstream 2025.11** — `git log` shows only `buildroot/configs/*` was ever modified after the import commit `375208e9`. | Do not patch upstream packages. Add ours out of the way (section 6). |
| `buildroot/Makefile` lines 830–843 | 14 lines | The **only** NeoDCT modification to upstream Buildroot: a `.PHONY: update` target that shells out to `neodct/tools/mkupdate.py`. Reads `NEODCT_UPDATE_THUMBNAIL ?= $(wildcard $(TOPDIR)/../neodct/release-thumbnail.png)`. | Keep verbatim. Nothing about it is Python-specific. |
| `buildroot/board/qemu/busybox.fragment` | 4 lines | `CONFIG_PING6=y` — T-Mobile is IPv6-only, `S45modem`'s connectivity check needs `ping6`. | Keep verbatim. |
| `buildroot/board/qemu/post-image.sh` | upstream | Generates `start-qemu.sh`. Runs *before* `post-image-neodct.sh` in the qemu defconfig's script list. | Keep. |
| `buildroot/board/qemu/aarch64-virt/linux.config` | upstream | Kernel config used by **both** defconfigs (yes, the ARM luckfox one points at the aarch64-virt config; see Risk R-11). | Untouched by this port. |
| `buildroot/configs/neodct_qemu_defconfig` | 85 lines | Primary dev target: aarch64, QEMU virt. | Edited (section 5). |
| `buildroot/configs/luckfox_pico_mini_defconfig` | 85 lines | Real hardware: `BR2_arm=y`, `BR2_cortex_a7=y`, `BR2_ARM_FPU_NEON=y`. | Edited (section 5). |
| `neodct/configs/*defconfig` | duplicates | **Second copy of both defconfigs.** Verified byte-identical to the `buildroot/` copies today (`diff` clean). The build uses the `buildroot/` copy; this one is documentation. | Both copies must be edited together. See Risk R-1. |
| `buildroot/{bg_home.h, composer.py, font.ttf, list.py, uidraw.py, notes.txt, ubinize-rootfs.cfg}` | 456 KB | Stray dev cruft at the Buildroot root from an early prototyping era. `font.ttf` is byte-identical (`md5 440b53b1…`) to the shipped `System/ui/resources/fonts/font.ttf`. Nothing in the build references any of them. | Not part of the port. Leave alone or delete separately; do **not** let a C agent "tidy" them as part of this work. |

### Build hooks — `neodct/scripts/`

| Path | LOC | Purpose | C destination |
| --- | --- | --- | --- |
| `neodct/scripts/post-build-prune-tests.sh` | 101 | busybox-ash post-build hook. Deletes `$TARGET_DIR/tests`; drops apps present in the target but absent from the overlay; deletes `etc/init.d/S50sshd`; deletes editor droppings; honours `NEODCT_EXCLUDE_APPS`; deletes `__pycache__`; swaps in `inittab.luckfox` on luckfox platforms. | **Stays shell.** Two edits: the `__pycache__` sweep becomes dead (harmless, keep for a transition period), and it should additionally strip debug symbols from the installed `.so`/binaries if the package does not. |
| `neodct/scripts/post-build-system-metadata.sh` | 107 | Writes `NeoDCT/System/version.prop` from `etc/os-release`'s `VERSION_ID`; creates `NeoDCT/User`; renders `/etc/neodct-banner` with figlet; rewrites `/etc/issue`. | **Stays shell, unchanged.** Nothing Python-specific. Its output contract is load-bearing (section 3.1). |
| `neodct/scripts/post-image-neodct.sh` | 156 | Post-image hook. Calls `mkupdate.py --image-only` (squashfs + verity + `installed.prop`), `mkinitramfs.py`, `mke2fs` for `userdata.ext4` (512 MB default, label `NDUSER`), `mkfs.vfat` + `mmd` for `sdcard.img` (128 MB, label `NEODCT`, folders `wallpapers tones backup_db music update`). | **Stays shell, unchanged.** It calls two Python host tools; host tools may stay Python (section 5.5). |
| `neodct/scripts/mkinitramfs.py` | 432 | Host tool. Hand-rolled 32/64-bit ELF reader (`elf_needed`, `elf_machine`), transitive `DT_NEEDED` closure, busybox applet symlinks, BMP→XRGB8888 splash conversion, cpio+gzip. | **Stays Python on the host.** One change: it copies `NeoDCT/System/hw/neodct_displayd` into the initramfs; that path must not move. |
| `neodct/scripts/qemu_modem_data_test.py` | 245 | Host-side modem data-path smoke test under QEMU. | Out of scope for this subsystem. |

### Test and capture tooling — `neodct/tools/`

| Path | LOC | Purpose | C destination |
| --- | --- | --- | --- |
| `neodct/tools/uistub.py` | 474 | The headless fake phone. `CapturingFramebuffer`, `PathRemap`, `KeyScript`, `ScriptExhausted`, `StubUI`, `stage_overlay()`, `run_app()`. | **Stays Python.** It is the *reference* side of the oracle and must keep driving the Python build unchanged for as long as both builds exist. The C side gets an independent equivalent, `nd-shoot` (section 6.4). |
| `neodct/tools/goldenframe.py` | 376 | Deterministic capture + comparison. `VirtualClock`, `_Frozen`, `instrument()`, `DeterministicUI`, `frame_digest()`, `write_manifest()`, `compare()`, `_describe_pixel_diff()`, `capture()`, CLI. | **Stays Python and becomes the umpire for both builds.** `--compare REF CAND` already works on any two directories, so it judges C output with no changes. Needs the layout-engine pin (3.9). |
| `neodct/tools/shoot_docs.py` | 357 | Defines *which* 49 screens exist and exactly how each is produced. `shoot_home`, `shoot_app_selector`, `shoot_stock_apps`, `shoot_games`, `shoot_telephony`, `shoot_engineering_apps`, `shoot_widgets`, `shoot_examples`. | **This file is the specification of the shot list.** Section 3.6 transcribes every recipe so the C `nd-shoot` can reproduce them without reading the Python. |
| `neodct/tools/run_qemu.sh` | 218 | Boots the image set under QEMU. Env-driven: `NEODCT_SNAPSHOT NEODCT_VERITY NEODCT_SD NEODCT_RECOVERY NEODCT_RECTTY NEODCT_MODEM NEODCT_NET NEODCT_DEBUG NEODCT_AUDIO NEODCT_DISPLAY NEODCT_MEM NEODCT_IMAGES NEODCT_SHARE NEODCT_MONITOR NEODCT_QEMU_EXTRA NEODCT_MODEM_VENDOR NEODCT_MODEM_PRODUCT`. Defaults: 72 MB RAM, `cortex-a53`, verity `enforce`, gtk display, PulseAudio. | **Unchanged.** It boots an image; it does not care what is in it. Add nothing. |
| `neodct/tools/sdcard.sh` | 106 | mtools wrapper: `ls put rm init new` against `sdcard.img` with no root or loop mount. | Unchanged. |
| `neodct/tools/release.sh` | 124 | Tag from `VERSION_ID`. | Unchanged. |
| `neodct/tools/mknand.sh` | 136 | Builds the raw-NAND image set for the Luckfox. | Unchanged. |
| `neodct/tools/mkicons.py` | 83 | Regenerates two app icons with PIL on a 40×40 grid, `SCALE=3`, `Image.NEAREST`. Design-time only, never runs in a build. | **Stays Python, host-only.** Not shipped, not ported. |
| `neodct/tools/mkt9dict.py` | 221 | Builds `t9.dict` (3,022,855 bytes) on the host. | Stays Python, host-only. |
| `neodct/tools/mkupdate.py`, `mkbadupdate.py` | 292, — | Update packaging. Covered by `spec-update-system.md`. | Host-only Python; see 5.5. |

### CI

| Path | LOC | Purpose | Fate |
| --- | --- | --- | --- |
| `.github/workflows/release.yml` | 115 | The **only** workflow. Fires on tags `0.*` / `v0.*`. Checks the tag equals `VERSION_ID` in `neodct/overlay/etc/os-release`, extracts the matching `CHANGELOG.txt` section with an awk block, appends a fixed signature footer, creates a **pre-release**. Attaches no `.ndsw`. | Keep. **There is no CI job that runs the tests.** Adding one is the highest-value change this subsystem can make for the port — section 8.1. |

### Tests — `neodct/tests/` (36 files, 9,399 lines, 659 `def test_` functions, 676 collected items)

Measured on this machine: `python3 -m pytest neodct/tests/ -q` → **657 passed, 17 failed,
2 skipped in 21.9 s**. All 17 failures are environmental in this sandbox, not
regressions: 11 need `libselinux.so.1` in one of the four directories `mkinitramfs`
searches, 5 need `sshd`/`ssh-keygen`, 1 is a date-formatting assertion. `cpio`,
`mksquashfs`, `veritysetup` and `figlet` are also absent here. **`AGENTS.md` says "510
tests, ~20s" — that is stale; it is 659/676 and 22 s.**

Full inventory in section 8.

---

## Behaviour that must be reproduced exactly

### 3.1 `version.prop` — the file the whole update system is checked against

`post-build-system-metadata.sh` writes `$TARGET_DIR/NeoDCT/System/version.prop`. Exact
content, exact key names, exact order:

```
# Generated by post-build-system-metadata.sh -- do not edit.
# Facts about this image. User settings are in /NeoDCT/User/settings.prop.
system.os.versionnumber=$VERSION
system.os.versionname=NeoDCT System v$VERSION
system.os.platform=$PLATFORM
system.os.buildtime=$BUILD_TIME
system.os.buildepoch=$EPOCH
```

Rules that are load-bearing and were each a shipped bug once:

- `$VERSION` comes **only** from `sed -n 's/^VERSION_ID=//p' $TARGET_DIR/etc/os-release | tr -d '"' | head -n1`.
  Empty ⇒ `exit 1` with `No VERSION_ID in ...` on stderr.
- Buildroot invokes post-build scripts as
  `script TARGET_DIR $BR2_ROOTFS_POST_SCRIPT_ARGS $BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS`.
  The qemu board needs the defconfig **path** in `POST_SCRIPT_ARGS`, so the path arrives
  as `$2` and the platform id is **last**. The script therefore does
  `shift; for argument in "$@"; do PLATFORM="$argument"; done`.
- Any `$PLATFORM` that is empty **or contains a `/`** becomes the literal string
  `unknown`. A build-machine path once landed in `system.os.platform`, and that value is
  what every update manifest is compared against, so updates built in a different
  directory would have been refused forever as "WRONG UPDATE FOR THIS PHONE".
- `$EPOCH` is `$SOURCE_DATE_EPOCH` when set, else `date -u '+%s'`.
  `$BUILD_TIME` is `date -u -d "@$EPOCH" '+%Y-%m-%d %H:%M UTC'`, falling back to
  `date -u -r "$EPOCH" ...` then bare `date -u ...`.
- `mkdir -p "$TARGET_DIR/NeoDCT/User"` — the mountpoint must exist inside the read-only
  squashfs; nothing can create it at runtime.
- `/etc/neodct-banner` = `figlet -f small "NeoDCT OS $VERSION"`, falling back to plain
  text if figlet is absent (not an error). `-f small` is 66 columns for this string;
  `standard` is 77 and wraps.
- `/etc/issue` is rewritten as `printf 'NeoDCT System v%s\n' "$VERSION"`, overriding
  `BR2_TARGET_GENERIC_ISSUE`, which was still advertising 0.3.0a at 0.3.5a.

Platform ids in use: **`qemu-aarch64`** and **`luckfox-armv7`**. These strings are baked
into `BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS` in the defconfigs and must not change — an
update package carries the platform id and the phone refuses a mismatch.

### 3.2 `post-build-prune-tests.sh` — what must not reach the image

In order:

1. `rm -rf "$TARGET_DIR/tests"`.
2. For each of `NeoDCT/System/apps` and `NeoDCT/System/engineering/apps`: any directory
   present in the target but absent from `neodct/overlay/<same path>` is deleted, with
   `[post-build] dropping stale app: <rel>/<name>` printed. Reason: `BR2_ROOTFS_OVERLAY`
   copies *over* the target tree and never deletes, and Buildroot does not rebuild
   `target/` between builds, so a deleted app stays installed until `make clean`. A
   deleted app shipped inside a signed update once already.
3. `rm -f "$TARGET_DIR/etc/init.d/S50sshd"` — openssh's own boot script starts sshd on
   every interface at every boot, and this phone has a public IPv6 address on mobile
   data. `System/core/RemoteShell` is the only thing allowed to start sshd.
4. `find "$TARGET_DIR/NeoDCT" \( -name '.~lock.*#' -o -name '*.swp' -o -name '*~' -o -name '.#*' \) -type f -print -delete`.
   A LibreOffice lock file reached a signed image this way once.
5. `NEODCT_EXCLUDE_APPS="Tube Foo" make` deletes those app directories from both app
   roots, printing `[post-build] excluding app: <rel>/<name>`.
6. `find "$TARGET_DIR/NeoDCT" -name __pycache__ -type d -prune -exec rm -rf {} +`.
7. If `${PLATFORM%%-*}` = `luckfox`, `cp etc/inittab.luckfox etc/inittab`. Either way,
   `rm -f etc/inittab.luckfox`.

**In the C port, step 6 becomes a no-op but steps 1–5 and 7 all still matter.** Step 2 in
particular becomes *more* important, not less: a stale `app.so` from a deleted app would
still be `dlopen()`-able by `nd-apprun`.

### 3.3 `post-image-neodct.sh` — the image set

Produces in `$BINARIES_DIR`: `system.img`, `system.manifest.json`, `initramfs.cpio.gz`,
`userdata.ext4`, `sdcard.img`. Exact behaviours worth preserving:

- Exits 0 with `no rootfs.squashfs -- enable BR2_TARGET_ROOTFS_SQUASHFS; skipping` if
  there is no squashfs.
- `host_tool()` prefers `$HOST_DIR/sbin/$1` then `$HOST_DIR/bin/$1` then `$PATH`.
- `USERDATA_MB=${NEODCT_USERDATA_MB:-512}`, `SDCARD_MB=${NEODCT_SDCARD_MB:-128}`.
- `mke2fs -q -F -t ext4 -b 4096 -L NDUSER -d "$SKEL" -E root_owner=0:0 "$USERDATA" $((USERDATA_MB * 256))`
  (blocks, at 4096 B/block). `root_owner=0:0` because everything on the phone runs as root.
- The `$SKEL` seeded into the fresh partition is exactly:
  `db logs .ndsys .pycache .seedrng sdcard tones wallpapers`.
  **`.pycache` becomes dead in the C port** — `run_neodct.sh` sets
  `PYTHONPYCACHEPREFIX=/NeoDCT/User/.pycache` and nothing else uses it. Leave the
  directory in the skeleton anyway during the transition; removing it is a one-line
  change once no Python remains.
- `NEODCT_KEEP_USERDATA=1` rewrites `.ndsys/installed.prop` in place with `debugfs`,
  then **reads the root hash back out and compares it**, exiting 1 on mismatch. The
  comment warns never to put a short-circuiting reader (`grep -q`) on debugfs's stdout —
  it SIGPIPEs debugfs mid-write and the image then carries the previous build's root hash
  and will not boot.
- `sdcard.img` is created **only if absent**, FAT32, label `NEODCT`, with folders
  `wallpapers tones backup_db music update` created via `mmd -i ... "::/$folder"` under
  `MTOOLS_SKIP_CHECK=1`.

### 3.4 `mkinitramfs.py` — the applet list and the splash

`APPLETS` is a fixed 51-entry tuple and `test_initramfs_applets.py` statically scans
every file in `neodct/initramfs/` for command names and fails if one is missing:

```
sh mount umount mkdir cat echo sleep dd sync sha256sum switch_root blkid ls rm mv cp
grep sed awk printf test true false dmesg mknod modprobe losetup head cut date expr
wc tr dirname basename mountpoint chmod ln unzip stty reboot clear mkfifo kill
```

Other exact behaviours:

- `DEFAULT_LIB_DIRS = ("lib", "usr/lib", "lib64", "usr/lib64")`; libraries are flattened
  into `lib/` in the archive, with symlinks `lib64 -> lib`, `usr/lib -> ../lib`,
  `usr/lib64 -> ../lib` so any loader search path resolves.
- `DMSETUP_CANDIDATES = ("usr/sbin/dmsetup", "sbin/dmsetup", "usr/bin/dmsetup", "bin/dmsetup")`,
  installed at `sbin/dmsetup`. Missing ⇒ `sys.exit`.
- `PANEL_DAEMON = "NeoDCT/System/hw/neodct_displayd"`, copied to `bin/neodct_displayd`
  **only if `elf_machine(panel) == elf_machine(busybox)`** — it is a prebuilt binary in
  the overlay and is present even in target trees of the wrong architecture.
- Splash: `SPLASH_IMAGES = (("splash/sadface.bmp", "splash.raw"), ("splash/bootlogo.bmp", "bootlogo.raw"))`,
  `SPLASH_W, SPLASH_H = 240, 175`. `bmp_to_xrgb8888()` requires **uncompressed 24-bit
  BMP** at exactly 240×175, emits **top-down XRGB8888 (bytes B,G,R,0)** because
  `neodctDisplay.c force_mode()` asks the fb for 32bpp and reads it as XRGB8888. BMP
  stores bottom-up when height is positive, so rows are flipped. Deliberately hand-rolled
  rather than using Pillow: this runs during `make`, where nothing guarantees PIL exists.
- Archive is `cpio --quiet -o -H newc | gzip -9 -n` over `find . -mindepth 1 -printf '%P\n'`.
- Staging directories created: `bin sbin lib usr proc sys dev mnt mnt/root mnt/user mnt/sdcard newroot`.

**None of this changes in the C port.** It is architecture-agnostic and libc-agnostic by
construction.

### 3.5 The golden-frame contract — this is the part the C build must implement

#### 3.5.1 Digest

`goldenframe.frame_digest(image)`:

```python
rgb = image if image.mode == "RGB" else image.convert("RGB")
h = hashlib.sha256()
h.update(b"%d,%d|" % (rgb.width, rgb.height))   # e.g. b"240,175|"
h.update(rgb.tobytes())                          # tightly packed R,G,B, top-down, no padding
return h.hexdigest()
```

Verified against the committed golden set: `240*175*3 = 126000` bytes for a band frame,
`240*240*3 = 172800` for a panel frame, and the digests reproduce exactly.

In C:

```c
char header[32];
int n = snprintf(header, sizeof header, "%d,%d|", w, h);
sha256_update(&ctx, header, (size_t)n);
sha256_update(&ctx, rgb_rows, (size_t)w * (size_t)h * 3u);   /* no row padding */
```

The hash is over **raw pixels, not the PNG file** — two encoders write different bytes
for identical images, and it is the pixels that matter. The PNG is written only so
`_describe_pixel_diff()` can show where the difference is.

#### 3.5.2 Manifest schema

`<dir>/manifest.json`, written with `json.dump(..., indent=2, sort_keys=True)`:

```json
{
  "epoch": 1704112496.0,
  "tick": 0.1,
  "seed": 20240101,
  "frames": [
    { "name": "app-calculator", "size": [240, 175], "sha256": "d6cd3e…" }
  ]
}
```

`frames` is sorted by `name` before writing. `compare()` reads **only** `frames`, and
within it only `name`, `size` and `sha256` — it does **not** verify that `epoch`, `tick`
or `seed` agree between the two directories (Risk R-6). Each `<name>.png` must also exist
next to the manifest.

`compare()` classifies each name as `extra` (in candidate only), `missing` (in reference
only), `size`, or `pixels`, and for `pixels` calls `_describe_pixel_diff()`, which prints
`"<n> px (<pct>%) in box (l, t, r, b)"` using `ImageChops.difference(...).getbbox()`.
Returns `(ok, diffs)`; the CLI exits 0/1.

#### 3.5.3 Determinism constants — changing any of these invalidates every stored frame

| Constant | Value | Why |
| --- | --- | --- |
| `goldenframe.EPOCH` | `1704112496.0` | 2024-01-01 12:34:56 UTC. Arbitrary but fixed forever. |
| `goldenframe.TICK` | `0.1` | One UI tick. Matches the 0.1 s `read_keypress` timeout in `core.main.run()`. |
| `goldenframe.SEED` | `20240101` | `random.seed()` value. |
| `TZ` | `"UTC"` | `time.strftime("%H:%M")` reads the local zone; a reference rendered in Dublin must match one rendered in a CI container. Set in `os.environ` **and** `time.tzset()`. |

#### 3.5.4 Virtual time semantics

The clock is **virtual, not frozen** — freezing outright would deadlock anything waiting
for time to pass (the `+CLIP` grace period in `poll_modem`, the cursor blink, the modem
retry backoff).

```
now() == EPOCH + frame * TICK          frame increments by 1 on each fb.update()
```

Three properties, all required:

- **deterministic** — frame N is always at `EPOCH + N*0.1` on every machine;
- **monotonic** — nothing polling for elapsed time can hang;
- **frame-aligned** — within one frame time does not move, so a screen composed of
  several draw calls cannot tear across a tick boundary.

`instrument(fb, clock)` wraps `fb.update` so the tick happens **after** the real update
returns. When the frame budget raises `ScriptExhausted`, the clock does *not* advance.

`_Frozen.PATCHED` — the complete list of `time` attributes replaced:

```
time, monotonic, monotonic_ns, time_ns, perf_counter, perf_counter_ns,
localtime, gmtime, strftime
```

Mappings:

| Patched | Behaviour |
| --- | --- |
| `time.time` / `time.monotonic` / `time.perf_counter` | `clock.now()` |
| `time.time_ns` / `monotonic_ns` / `perf_counter_ns` | `int(clock.now() * 1e9)` |
| `time.gmtime(secs=None)` | `real_gmtime(clock.now() if secs is None else secs)` |
| `time.localtime(secs=None)` | **`real_gmtime(...)`** — localtime is aliased to gmtime |
| `time.strftime(fmt, t=None)` | `real_strftime(fmt, real_gmtime(clock.now()) if t is None else t)` |

`perf_counter` is on the list specifically because **CubeBench integrates its rotation
angle over `perf_counter` deltas** — miss it and the cube lands at a different angle on a
faster machine.

The C equivalent needs the same three clock sources under one virtual clock, advanced by
the frame-commit function, with `TZ=UTC` and `gmtime_r` used where the Python uses
`localtime`.

#### 3.5.5 `random`

`random.seed(20240101)` is called in `_Frozen.__enter__`, i.e. **once per `StubUI`
context**, not once per capture run. Every shot function that opens its own `with
StubUI(...)` therefore starts from the same RNG state. The C port needs a PRNG whose
sequence matches Python's Mersenne Twister **only** where a random value reaches a
pixel — today that is Snake's food placement and Memory's card shuffle. See Risk R-7:
matching CPython's MT19937 *and* the exact consumption pattern of `random.randint` /
`random.shuffle` is a real cost, and the cheaper answer is to make those two games take
their randomness from an injectable source that both builds can pin.


### 3.6 The 49 shots — exact recipes

`shoot_docs.py` is the definition of the reference set. Transcribed here so a C agent
never needs the Python.

**Key codes** (evdev, as `shoot_docs.py` defines them):

```
UP=103  DOWN=108  LEFT=105  RIGHT=106  ENTER=28  BACK=14  STAR=42  HASH=43
DIGIT = {1:2, 2:3, 3:4, 4:5, 5:6, 6:7, 7:8, 8:9, 9:10, 0:11}
```

**Common setup abbreviations used in the table:**

- `WP` = `StubUI(wallpaper="Palestine.jpg")`
- `PLAIN` = `StubUI()` (no wallpaper)
- `STATUS` = `ui.stub.simulate_status(battery=4, signal=4, carrier="Tello")`, which sets
  `ui.battery.hardware = True`, `ui.battery._level = 4`,
  `ui.modem.signal_level = lambda: 4`, `ui.modem.operator_display = lambda: "Tello"`.
  Without it the home screen honestly draws `?` for battery and `No Service`.
- Every `StubUI` defaults to `engineering=True`, `skip_notice=True` (writes
  `/NeoDCT/User/.ack_security_warning` containing `0`), `idle_budget=60`.
- `run_app(ui, name, keys, frame_budget)` mirrors `NeoDCT_UI.launch_app` but lets
  `ScriptExhausted` through; returns `ui.fb.frames[start:]`. Default budget 240.
- Unless stated, the saved image is `ui.fb.frames[-1]`.

`ui.apps` is scanned from `/NeoDCT/System/apps` then, when `engineering_mode`, from
`/NeoDCT/System/engineering/apps`, and sorted by integer `id`. The order observed today:

```
 0 id=1    Phone book      8 id=10   Koki Mobile   16 id=9003 KeyMapI2C
 1 id=2    Messages        9 id=11   Browser       17 id=9004 FuelGauge
 2 id=3    Call Log       10 id=12   Update        18 id=9005 ModemInfo
 3 id=4    Settings       11 id=970  Music         19 id=9006 Downgrade
 4 id=6    Games          12 id=971  Power         20 id=9990 Remote Shell
 5 id=7    Calculator     13 id=999  Linux Shell   21 id=9997 Crash
 6 id=8    Clock          14 id=9001 LCD Test      22 id=9998 Cube Bench
 7 id=9    Tones          15 id=9002 KeyMap        23 id=9999 Tests
```

#### Group 1 — `shoot_home`

| Name | Size | Recipe |
| --- | --- | --- |
| `home` | 240×175 | `WP` + `STATUS`; `ui.update()`; save `frames[-1]`. |
| `home-panel` | **240×240** | Same UI, same moment; save `ui.fb.device_frame()`. |
| `home-dialing` | 240×175 | Same UI, continuing: `ui.handle_input(c)` for c in `[11, 8, 5, 2, 3, 4, 5, 6, 7, 8]` (digits `0 7 4 1 2 3 4 5 6 7`), then `ui.update()`. |
| `home-nowallpaper` | 240×175 | Fresh `PLAIN` + `STATUS`; `ui.update()`. |
| `home-simulation` | 240×175 | Fresh `WP`, **no `STATUS`** (the honest QEMU/dev look: `?` battery, `No Service`); `ui.update()`. |

#### Group 2 — `shoot_app_selector`

One `WP` + `STATUS` UI. `selector = AppSelector("Main Menu", ui.apps, ui, background=ui.wallpaper)`.
For each wanted name, set `selector.selected_index` to that app's index in `ui.apps`,
call `selector.draw()`, save as `menu-<name.lower().replace(" ", "-")>`.

Wanted, in this order: `Phone book, Messages, Games, Settings, Calculator, Koki Mobile,
Browser, Music`. Names not present in `ui.apps` are skipped.

| Name | Size | Note |
| --- | --- | --- |
| `menu-phone-book` | 240×175 | index 0 |
| `menu-panel` | **240×240** | `ui.fb.device_frame()` taken immediately after `menu-phone-book` |
| `menu-messages` | 240×175 | index 1 |
| `menu-games` | 240×175 | index 4 |
| `menu-settings` | 240×175 | index 3 |
| `menu-calculator` | 240×175 | index 5 |
| `menu-koki-mobile` | 240×175 | index 8 |
| `menu-browser` | 240×175 | index 9 |
| `menu-music` | 240×175 | index 11 |

#### Group 3 — `shoot_stock_apps`

Each case gets its **own fresh** `WP` + `STATUS` UI. `run_app(ui, name, keys, budget)`,
save `frames[-1]`. Any exception is caught, printed as `!! <slug>: <Type>: <msg>`, and
the shot skipped.

| Name | Manifest name | Keys | Budget |
| --- | --- | --- | --- |
| `app-phonebook` | `Phone book` | `[]` | 240 |
| `app-messages` | `Messages` | `[]` | 240 |
| `app-messages-inbox` | `Messages` | `[28]` | 240 |
| `app-calllog` | `Call Log` | `[]` | 240 |
| `app-settings` | `Settings` | `[]` | 240 |
| `app-settings-wallpaper` | `Settings` | `[28]` | 240 |
| `app-games` | `Games` | `[]` | 240 |
| `app-calculator` | `Calculator` | `[2, 3, 4]` (digits 1,2,3) | 240 |
| `app-calculator-options` | `Calculator` | `[8, 28]` (digit 7, Enter) | 240 |
| `app-clock` | `Clock` | `[]` | 240 |
| `app-tones` | `Tones` | `[]` | 240 |
| `app-musicplayer` | `Music` | `[]` | 240 |
| `app-koki` | `Koki Mobile` | `[]` | **400** — Koki never polls `read_keypress`, so only the frame budget stops it; ~60 frames reaches the title card |

#### Group 4 — `shoot_games`

Fresh `PLAIN` + `STATUS` per case, `run_app(ui, "Games", keys, budget)`.
The Games menu lists **Memory then Snake**.

| Name | Keys | Budget |
| --- | --- | --- |
| `game-snake` | `[108, 28, 28]` (Down, Enter, Enter) | 300 |
| `game-memory` | `[28, 28]` (Enter, Enter) | 300 |

#### Group 5 — `shoot_telephony`

One `WP` + `STATUS` UI for the first four, then a separate `PLAIN` UI for the crash
screen. The `draw_*` helpers only paint the canvas — the loops that own them normally
flush — so the harness calls `ui.fb.update(ui.canvas)` by hand.

| Name | Recipe |
| --- | --- |
| `call-active` | `System.ui.Dialer.call_screen.draw_call_screen(ui, "0741234567", name="Mum")` then `ui.fb.update(ui.canvas)`. |
| `call-incoming` | `System.ui.Dialer.incoming_screen.draw_incoming_screen(ui, "Mum", True)` then `ui.fb.update(ui.canvas)`. |
| `home-sms-banner` | `ui.notify.post_sms(1, tone=False)`; `ui._unread_sms = 1`; `ui.update()`. The 3310-style "N message(s) received" banner plus the flashing envelope. |
| `contacts-picker` | `ui.keys.push(14)`; `System.apps.PhoneBook.shared.list_ui.show_contact_selector(ui, title="Select", btn_text="Call")` inside `try/except BaseException`; save `frames[-1]`. |
| `crash-screen` | Fresh `PLAIN`, **no `STATUS`**. `raise RuntimeError("example failure")`, caught, then `System.core.CrashHandler._draw_engineering_crash_screen(ui, "RuntimeError: example failure")`, then `ui.fb.update(ui.canvas)`. |

#### Group 6 — `shoot_engineering_apps`

Fresh `PLAIN` + `STATUS` per case (note: **no wallpaper**), default budget 240.

| Name | Manifest name |
| --- | --- |
| `eng-modem` | `ModemInfo` |
| `eng-fuelgauge` | `FuelGauge` |
| `eng-lcdtest` | `LCD Test` |
| `eng-cubebench` | `Cube Bench` |
| `eng-tests` | `Tests` |

#### Group 7 — `shoot_widgets`

**One** `PLAIN` + `STATUS` UI for all ten, drawn in this order (order matters: the
virtual clock advances across them and each widget draws onto the same canvas).

| Name | Recipe |
| --- | --- |
| `widget-verticallist` | `vlist = VerticalList(ui, "Phonebook", ["Search", "Add entry", "Edit", "Erase", "Send entry", "Options"], app_id=1)`; `SoftKeyBar(ui).update("Select", present=False)`; `vlist.draw()`. |
| `widget-verticallist-scrolled` | `vlist.selected_index = 2`; `SoftKeyBar(ui).update("Select", present=False)`; `vlist.draw()`. |
| `widget-pagedlist` | `PagedList(ui, "Messages", ["Inbox", "Outbox", "Write Message"], root_id=2).draw()`. |
| `widget-textinput` | `TextInput(ui, "Phonebook", "Name:", initial_text="Sam").draw()`. |
| `widget-textinputlong` | `t = TextInputLong(ui, "Write Message")`; `t.set_text("Meet me by the old phone box at six")`; `t.draw()`. |
| `widget-messagedialog` | `MessageDialog(ui, "This application has not been implemented yet.").render()`. |
| `widget-textscroller` | `TextScroller(ui, "Feed the snake by steering it to the food. Every bite makes it grow longer. Use keys 2, 4, 6 and 8 to change direction.").draw()`. |
| `widget-levelselector` | `LevelSelector(ui, current=3).draw()`. |
| `widget-infoscreen` | `info = InfoScreen(ui, "Top score", 1250)`; `ui.keys.push(14)`; `info.show()` in `try/except BaseException`. |
| `widget-softkeybar` | `header = HeaderWidget(ui, 3)`; `ui.draw.rectangle((0, 0, ui.W, ui.H), fill="black")`; `ui.draw.text((5, 0), "Call log", font=ui.font_xl, fill="white")`; `header.draw(2)`; `ui.draw.line((0, 30, ui.W, 30), fill="white")`; `SoftKeyBar(ui).update("Options")`. |

#### Group 8 — `shoot_examples` — **currently produces nothing**

Looks for tutorial apps at `<out>/../../examples` (default `<repo>/../neodct-docs/examples`),
which does not exist in this checkout, so it prints `!! no examples at …` and returns.
The five names it *would* produce — `example-hello`, `example-dice`,
`example-countdown-menu`, `example-countdown`, `example-dice-menu` — are absent from the
golden set. If those tutorial apps are ever restored, the reference set grows to 54 and
the C `nd-shoot` must handle installing an app from a directory at runtime.

#### Three golden frames are byte-identical to three others

Verified, and legitimate — the widget gallery reproduces exactly what those apps draw:

| Pair | Digest |
| --- | --- |
| `app-clock` ≡ `widget-messagedialog` | `e9dccc0c2f46…` (the Clock app *is* a "This application has not been implemented yet." dialog) |
| `app-messages` ≡ `widget-pagedlist` | `38c5a2eb44ec…` |
| `app-phonebook` ≡ `widget-verticallist` | `d7bbbc8a76b8…` |

So the 49 names cover 46 distinct images. Do not treat a duplicate digest as a capture bug.

### 3.7 Geometry — and a divergence between the stub and the hardware

| Constant | Value | Source |
| --- | --- | --- |
| `UI_WIDTH` / `ui.W` | 240 | `core/main.py:39` |
| `UI_HEIGHT` / `ui.H` | 175 | `core/main.py:40` |
| `SOFTKEY_HEIGHT` / `ui.SOFTKEY_H` | 30 | `core/main.py:41` |
| `ui.content_bottom` | 145 (`H - SOFTKEY_H`) | `core/main.py:537` |
| `ui.canvas` | `Image.new("RGB", (240, 175), "black")` | `core/main.py:599` |
| Panel | 240×240 | `uistub.PANEL_W/PANEL_H`, `neodctDisplay.c` |

**The divergence.** `CapturingFramebuffer.device_frame()` **centres** the band:

```python
panel.paste(band, ((240 - 175) // 2, ...))     # y = 32
```

and `test_uistub.py::test_device_frame_band_starts_at_row_32` pins that. But the real
hardware daemon **bottom-aligns** it:

```c
#define DEFAULT_Y_OFFSET (PANEL_H - FB_H)   /* 65 */
static int opt_yoff = DEFAULT_Y_OFFSET;
```

and `S90display` starts it with no `--yoff`, i.e. 65. The Nokia faceplate window is at
the bottom of the panel; the top 65 rows stay black.

Consequences:

- `home-panel` and `menu-panel` — the only two 240×240 golden frames — are **documentation
  aids, not device output**. The C `nd-shoot` must reproduce the *stub's* y=32 centring
  for those two names, because that is what the reference contains. Do **not** "fix" it
  to 65; that would break the oracle and change nothing on the phone.
- Every real correctness question is answered by the 47 band frames (240×175). Prefer
  those. Consider marking the two panel frames as informational in a future manifest
  revision.

### 3.8 Pillow's exact pixel arithmetic — measured, not assumed

The oracle is a SHA-256 over raw pixels, so the C rasterizer must match Pillow's integer
rounding exactly. These were determined empirically against Pillow 12.3.0 on this
machine, not recalled from documentation.

#### 3.8.1 Text / bitmap compositing — `ImageDraw.text()` and `ImageDraw.bitmap()`

Per channel, where `mask` is the 8-bit FreeType coverage value:

```
out = (dst * (255 - mask) + ink * mask + 127) / 255        (integer division, truncating)
```

Equivalently `round_half_up(dst + (ink - dst) * mask / 255.0)`.

Verified with **zero mismatches over 227,328 combinations** (dst 0..255 step 7, ink 0..255
step 11, mask 0..255). Three plausible alternatives were tested and all fail:

| Candidate | Mismatches | First failure (dst, ink, mask, pillow, candidate) |
| --- | --- | --- |
| `(dst*(255-m) + ink*m + 127) / 255` | **0** | — |
| `round(dst + (ink-dst)*m/255)` (float) | **0** | — |
| `(dst*(255-m) + ink*m + 128) / 255` | 446 | `(0, 11, 197) → 8`, candidate `9` |
| Pillow's `MULDIV255` BLEND macro | 52,910 | `(7, 11, 12) → 7`, candidate `8` |
| `dst + MULDIV255(ink-dst, m)` | 220 | `(7, 0, 164) → 2`, candidate `3` |

C:

```c
static inline uint8_t nd_blend8(uint8_t dst, uint8_t ink, uint8_t mask)
{
    /* Pillow ImagingFill2: measured exactly, +127 then truncating /255.
     * NOT the MULDIV255 macro -- that differs on 52910 of 227328 inputs. */
    return (uint8_t)(((uint32_t)dst * (255u - mask)
                    + (uint32_t)ink * mask + 127u) / 255u);
}
```

#### 3.8.2 Wallpaper dimming — `ImageEnhance.Brightness(img).enhance(0.3)`

**Truncation, not rounding**, and therefore a *different* convention from 3.8.1 in the
same codebase. Per channel:

```
out = (uint8_t)(v * 0.3)          /* C double multiply, cast truncates toward zero */
```

Verified over all 256 input values: `int(v*0.3)` matches with 0 mismatches;
round-half-up mismatches on 128 of 256. Samples: `1→0, 2→0, 3→0, 5→1, 17→5, 85→25,
128→38, 255→76`.

Getting this wrong changes **every wallpapered frame** — that is 30 of the 49 goldens.

#### 3.8.3 `Image.resize()` short-circuits at identical size

`load_wallpaper()` (`core/main.py:683`) does
`img.resize((240, 175), Image.Resampling.LANCZOS)`. Pillow returns `self.copy()` when the
requested size equals the current size, so **LANCZOS never runs for `Palestine.jpg`**
(240×175). Verified: the resize is a byte-identical identity.

That is a coverage hole. The other stock wallpapers are 240×240 (`90s throwback`,
`Dark Fantasy`, `Fruitiger Aero`) and 240×170 (`Digital Swamp`, `Grasslands`), and those
*do* run LANCZOS — but **no golden frame uses them**, so a C implementation of LANCZOS
could be arbitrarily wrong and the oracle would pass. See Risk R-4 and the proposed
`home-wallpaper-<name>` additions in section 8.4.

#### 3.8.4 Wallpaper pipeline, in order

```
ImageFile.LOAD_TRUNCATED_IMAGES = True      # global, set on every load
Image.open(path); img.load()
img.convert("RGB")
img.resize((240, 175), LANCZOS)             # identity when already 240x175
ImageEnhance.Brightness(img).enhance(0.3)
```

Any failure returns `None` and the home screen falls back to black.

#### 3.8.5 Fonts

`core/main.py:606-609`, one file, four sizes, **no `layout_engine` argument**:

```python
self.font_s  = ImageFont.truetype(font_path, 14)
self.font_md = ImageFont.truetype(font_path, 18)
self.font_n  = ImageFont.truetype(font_path, 20)
self.font_xl = ImageFont.truetype(font_path, 24)
```

Font file: `/NeoDCT/System/ui/resources/fonts/font.ttf`, 16,272 bytes,
`md5 440b53b1a1c65037f944ff19259d8014`, family **"Nokia Cellphone FC"**, style "Small".
At size 24 its ascent/descent is `(24, 6)`.

Fallback when the file cannot be loaded: all four become `ImageFont.load_default()` — a
built-in Pillow bitmap font that would need its own C reproduction. In practice the file
is always present; the C port should log loudly and abort rather than silently substitute.

Text is **antialiased**. Measured coverage values in the shipped font at size 18 for
`"Agy%"`: `{0, 40, 52, 128, 154, 255}`. A typical golden band frame contains
5–8 distinct colours: black, white, and 3–6 antialiasing greys such as
`(152,152,152)`, `(128,128,128)`, `(104,104,104)`, `(76,76,76)`, `(44,44,44)`,
`(216,216,216)`. There is no "it's a pixel font, so it's 1-bit" shortcut.

### 3.9 The layout-engine problem — read this before trusting the committed goldens

`ImageFont.truetype()` with no `layout_engine` picks **`Layout.RAQM` when Pillow was
built with libraqm, otherwise `Layout.BASIC`**.

- **This host:** Pillow 12.3.0, `features.check("raqm") == True` (libraqm 0.10.5),
  FreeType 2.14.3 → the golden set was captured with **RAQM**.
- **The phone:** `buildroot/package/python-pillow/python-pillow.mk` sets
  `PYTHON_PILLOW_BUILD_OPTS += -Craqm=disable` unconditionally, and there is no
  `libraqm` package in the Buildroot tree at all → the phone always uses **BASIC**.
  Buildroot's FreeType is **2.14.1**.

The two engines lay out this font differently. Measured glyph-mask widths for identical
strings:

| Size | Text | BASIC | RAQM |
| --- | --- | --- | --- |
| 14 | `Messages` | 85 px | 82 px |
| 14 | `Phone book` | 104 px | 100 px |
| 14 | `0741234567` | 106 px | 102 px |
| 18 | `Messages` | 103 px | **106 px** |
| 18 | `0741234567` | 126 px | **131 px** |
| 20 | `Phone book` | 152 px | 143 px |
| 20 | `Write Message` | 187 px | 178 px |
| 24 | (all tested strings) | identical | identical |

Note the sign flips between 14/20 (BASIC wider) and 18 (RAQM wider), so this is not a
uniform scale factor that could be compensated for.

**Impact, measured.** I recaptured the whole reference set with
`PIL.ImageFont.core.HAVE_RAQM = False` forced before any font is constructed, and
compared against the committed `neodct/tests/golden/`:

```
46 of 49 frames differ
  widget-textscroller   7059 px (16.81%) in box (20, 10, 227, 171)
  eng-tests             2620 px ( 6.24%) in box (11, 65, 225, 107)
  app-clock             2485 px ( 5.92%) in box (20, 70, 216, 103)
  app-settings          2186 px ( 5.20%) in box (41,  7, 233, 171)
  …
  home-dialing           110 px ( 0.26%) in box (96,153, 141, 171)
```

Only `game-memory`, `game-snake` and `app-koki` are unaffected.

**What to do, before any C is written:**

1. Pin the layout engine in `goldenframe.py`, in `_Frozen.__enter__` (so it is restored
   on exit like everything else):

   ```python
   # The phone's Pillow is built with -Craqm=disable (see python-pillow.mk), so it
   # always uses Layout.BASIC. A desktop Pillow with libraqm picks RAQM and lays this
   # font out 3-10 px differently per string -- 46 of 49 reference frames change.
   # Pin it, or the reference describes the developer's laptop rather than the phone.
   from PIL import ImageFont
   self._saved_raqm = ImageFont.core.HAVE_RAQM
   ImageFont.core.HAVE_RAQM = False
   ```

   (Setting `HAVE_RAQM` is what `FreeTypeFont.__init__` consults; passing
   `layout_engine=` at each call site would mean touching `core/main.py`, which is
   shipped code, so the harness-side pin is the right lever.)

2. Recapture: `python3 neodct/tools/goldenframe.py --out neodct/tests/golden/`.
3. Add an assertion to the capture path that refuses to write a manifest if
   `ImageFont.core.HAVE_RAQM` is true, so this cannot regress silently.
4. Record the FreeType version in the manifest (`"freetype": "2.14.3"`) and have
   `compare()` warn — not fail — on a mismatch. Host FreeType 2.14.3 vs Buildroot
   2.14.1 is a residual, unquantified risk (R-3).


---

## Public interface (the functions other parts call)

This subsystem is mostly *contracts between programs*, not C functions. Four of them.

### 4.1 The `nd-shoot` CLI — what the C build must provide

```
nd-shoot --out DIR [--only NAME[,NAME…]] [--list]
```

Obligations:

1. Render the named frames (all 49 by default) from the **real** C UI code — the same
   `libneodct.so` widgets and the same core loop that ship on the phone. A separate
   "test renderer" proves nothing.
2. Write each as `DIR/<name>.png`, RGB, no alpha channel.
3. Write `DIR/manifest.json` in exactly the schema of 3.5.2, `frames` sorted by name,
   `sha256` computed per 3.5.1.
4. Run entirely offline, with no `/NeoDCT` on the host: it needs the same path-remapping
   trick `PathRemap` performs. Simplest C equivalent: an `ND_ROOT` environment variable
   consulted by one `nd_path_resolve()` function that every file open in `libneodct.so`
   goes through, defaulting to `/NeoDCT`. **That function must exist anyway** — the
   overlay's absolute paths are load-bearing (`AGENTS.md`) and the port needs one choke
   point for them. Introduce it early; retrofitting it across 70 call sites is misery.
5. Stage a writable copy of the overlay, exactly as `uistub.stage_overlay()` does,
   skipping `__pycache__`, `*.pyc` and **`t9.dict`** (3,022,855 bytes of read-only word
   list; `t9_dict` falls back to no suggestions when it is absent, which is exactly the
   behaviour of a phone without one).
6. Implement the virtual clock of 3.5.4, advanced by the frame-commit call.
7. Honour a frame budget per app (`--budget`), because Koki and the games never read the
   keypad and only a draw-count limit stops them.
8. Exit non-zero if any requested frame could not be produced. `shoot_docs.py` swallows
   per-shot exceptions and prints `!! <slug>: …`; **do not copy that** in the C tool —
   a silently missing frame reads as `missing` in `compare()` and is easy to skim past.

`--list` prints the frame names one per line, so CI can assert the C build knows about
all 49 before it runs them.

### 4.2 `goldenframe.py` — the umpire, unchanged

```
python3 neodct/tools/goldenframe.py --out DIR                 capture the Python reference
python3 neodct/tools/goldenframe.py --compare REF CAND        judge two directories
python3 neodct/tools/goldenframe.py --verify-determinism DIR  capture twice into DIR/run-a
                                                              and DIR/run-b and compare
```

`--compare` is already build-agnostic: it reads two manifests and two sets of PNGs. It
needs **no changes** to judge C output. Exit 0 = identical, 1 = differences.

Python API worth keeping stable, because CI and future tooling call it directly:

```python
frame_digest(image) -> str
write_manifest(out_dir, entries) -> path
load_manifest(dir) -> dict
compare(reference_dir, candidate_dir, verbose=True) -> (ok, [(name, kind, detail), …])
capture(out_dir) -> entries
```

### 4.3 Buildroot package variables (what the rest of the build sees)

A package named `neodct` exposes, by Buildroot convention:

```
BR2_PACKAGE_NEODCT                     Kconfig symbol the defconfig sets
$(NEODCT_DIR)                          build directory
$(NEODCT_TARGET_INSTALL)               staged install into $(TARGET_DIR)
make neodct-rebuild                    re-sync sources and rebuild just this package
make neodct-reconfigure                as above, from configure
make neodct-dirclean                   throw the build dir away
```

With `NEODCT_SITE_METHOD = local`, Buildroot sets `NEODCT_OVERRIDE_SRCDIR` to
`NEODCT_SITE` (verified at `package/pkg-generic.mk:645`) and `rsync -au` s the tree in on
every build, skipping download, extract and patch entirely. `make neodct-rebuild` is then
the whole edit-compile-test loop and takes seconds.

### 4.4 Build entry points that must keep working

```sh
cd buildroot
make neodct_qemu_defconfig      # or luckfox_pico_mini_defconfig
make
neodct/tools/run_qemu.sh
NEODCT_SIGN_KEY=…/neodct-dev.key make update
```

Nothing in the port may change these four lines. They are in `AGENTS.md`, the README and
`docs/TESTING_UPDATES.md`.

---

## External dependencies and their C replacements

### 5.1 What the defconfigs select today

Both defconfigs are 85 lines and identical in the areas below. Differences: qemu is
`BR2_aarch64` with `BR2_GLOBAL_PATCH_DIR`, `BR2_DOWNLOAD_FORCE_CHECK_HASHES`,
`BR2_TOOLCHAIN_BUILDROOT_CXX`, ext4 2G rootfs and squashfs-zstd; luckfox is
`BR2_arm` + `BR2_cortex_a7` + `BR2_ARM_FPU_NEON` with squashfs-xz, ubifs and ubi.
Both point `BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE` at
`board/qemu/aarch64-virt/linux.config` and both build kernel 6.12.47.

Neither defconfig selects a C library, so Buildroot's default applies: **glibc**
(`toolchain/toolchain-buildroot/Config.in:25`). Neither selects an optimization level, so
the default applies: **`-O2`** (`Config.in:521`).

| Symbol | Pulls in | Needed after the port? |
| --- | --- | --- |
| `BR2_PACKAGE_PYTHON3` | libpython3.13, the stdlib, **libffi**, host-python3 | **No** |
| `BR2_PACKAGE_PYTHON3_SSL` | `openssl` + `OPENSSL_FORCE_LIBOPENSSL` + `LIBOPENSSL_ENABLE_BLAKE2` | Symbol no; **openssl yes** — see 5.3 |
| `BR2_PACKAGE_PYTHON3_SQLITE` | `sqlite` (3.51.1) | Symbol no; **sqlite absolutely yes** — see 5.3 |
| `BR2_PACKAGE_PYTHON3_XZ` | `xz` (liblzma + xz binaries) | Probably no — nothing in the runtime imports `lzma`; verify with `make show-info` |
| `BR2_PACKAGE_PYTHON_PILLOW` | `python3-pyexpat`→**expat**, `python3-zlib`→**zlib**, freetype, jpeg | **No** — replaced by the in-house rasterizer |
| `BR2_PACKAGE_PYTHON_MUTAGEN` | pyexpat, zlib (pure Python otherwise) | **No** — replaced by a small ID3v2 reader |
| `BR2_PACKAGE_PYTHON_MINIAUDIO` | `python-cffi` → **libffi**, host-python-cffi, host-python-pycparser | **No** — replaced by vendored `miniaudio.h` |
| `BR2_PACKAGE_FREETYPE` (2.14.1) | — | **Yes, keep.** Same renderer, same font, same sizes, same pixels. |
| `BR2_PACKAGE_JPEG` → `jpeg-turbo` 3.1.2 | NEON SIMD auto-selected on cortex-a7 (`BR2_ARM_CPU_HAS_NEON`) and aarch64 | **Yes, keep** — wallpapers are JPEG |
| `BR2_PACKAGE_OPENSSH` (+client, server, key-utils) | `openssl`, `zlib`, `libxcrypt` | Keep (engineering Remote Shell). **It is why openssl and zlib survive today.** |
| `BR2_PACKAGE_LIBCURL` (+`CURL`) | openssl by default (`BR2_PACKAGE_LIBCURL_OPENSSL`) | Keep |
| `BR2_PACKAGE_NETSURF` (+`FRAMEBUFFER`) | the browser — **netsurf-fb, not WebKitGTK** | Keep, out of scope |
| ~~`BR2_PACKAGE_LINKS` (+`GRAPHICS`)~~ | a second **graphical** web browser, never launched | **Removed** — SECURITY-PLAN.md section 4. Nothing in the tree runs it; every apparent reference is the English word. It shipped a whole second HTML/CSS/image parser and TLS client, as root, that nothing could reach. |
| `BR2_PACKAGE_MPV`, `BR2_PACKAGE_MPG123`, `BR2_PACKAGE_ALSA_UTILS` (+alsamixer, amixer, aplay) | external audio players Koki falls back to | Keep — `Koki/engine.py:340` probes `{"aplay", "mpg123", "mpv"}` |
| `BR2_PACKAGE_DEJAVU` | the font | Keep |
| ~~`BR2_PACKAGE_GPM`~~ | a console **mouse** daemon, on a phone with no pointer | **Removed** — SECURITY-PLAN.md section 4. Its only consumer in the enabled set was `LINKS_GRAPHICS`; netsurf-fb has no reference to it in the package, the makefile or the vendored source, and `netsurf-fb` carries no `libgpm` DT_NEEDED. ncurses `dlopen`s it and does not need it. |
| `BR2_PACKAGE_UQMI` | modem data session | Keep |
| `BR2_PACKAGE_LVM2` **without** `STANDARD_INSTALL` | dmsetup + libdevmapper only (~400 KB) instead of the whole LVM suite; the initramfs needs it for the verity table | Keep exactly as-is |
| `BR2_PACKAGE_DOSFSTOOLS` (+mkfs.fat, fatlabel, fsck.fat) | FAT32 SD cards | Keep |
| `BR2_PACKAGE_CA_CERTIFICATES`, `BR2_TARGET_TZ_INFO`, `BR2_ROOTFS_DEVICE_CREATION_DYNAMIC_EUDEV` | — | Keep |
| `BR2_PACKAGE_HOST_E2FSPROGS`, `HOST_DOSFSTOOLS`, `HOST_MTOOLS` | host tools used by `post-image-neodct.sh` | Keep |

**The browser is netsurf-fb, not WebKitGTK under cage.** `ARCHITECTURE.md` says
WebKitGTK/`cage`; `apps/Browser/main.py` line 3 says "NetSurf framebuffer browser
(netsurf-fb, NeoDCT chrome)" and the defconfigs select `BR2_PACKAGE_NETSURF` +
`BR2_PACKAGE_NETSURF_FRAMEBUFFER` with no `cage` or `webkitgtk` anywhere. Flagging this as
a conflict with the stated architecture; the conclusion ("out of scope, unchanged") is
unaffected.

### 5.2 Lines to delete

```diff
-BR2_PACKAGE_PYTHON3=y
-BR2_PACKAGE_PYTHON3_SSL=y
-BR2_PACKAGE_PYTHON3_SQLITE=y
-BR2_PACKAGE_PYTHON3_XZ=y
-BR2_PACKAGE_PYTHON_PILLOW=y
-BR2_PACKAGE_PYTHON_MUTAGEN=y
-BR2_PACKAGE_PYTHON_MINIAUDIO=y
```

Delete these **last**, not first. Until every app is C, the image must carry both.

### 5.3 Lines to add — and the two that are load-bearing

```diff
+# NeoDCT itself, now a C package (see package/neodct/).
+BR2_PACKAGE_NEODCT=y
+
+# Was reached only through BR2_PACKAGE_PYTHON3_SQLITE. Contacts, messages and
+# the call log are sqlite databases; dropping python3 without this ships a
+# phone with no phonebook.
+BR2_PACKAGE_SQLITE=y
+
+# Was reached only through PYTHON3_SSL / PYTHON_PILLOW / openssh. Made
+# explicit so the image does not silently lose TLS and PNG the day somebody
+# turns Remote Shell off.
+BR2_PACKAGE_OPENSSL=y
+BR2_PACKAGE_ZLIB=y
+
+# 277 PNG icons. Pillow decoded PNG itself on top of zlib, so libpng has
+# never been in this image.
+BR2_PACKAGE_LIBPNG=y
+
+# 8 MB of RAM and 100 MiB of NAND. -Os over -O2 across the whole image.
+BR2_OPTIMIZE_S=y
```

> **`BR2_PACKAGE_SQLITE=y` is the one that will be forgotten.** It is selected today
> *only* by `BR2_PACKAGE_PYTHON3_SQLITE`. Delete the Python lines without adding it and
> the build succeeds, the image boots, and the phonebook, messages and call log are gone.

**PNG is lossless**, so any conforming decoder produces identical pixels to Pillow's —
libpng 1.6.53 is a safe, boring choice at roughly 200 KB. JPEG is *not* in that category:
see Risk R-5.

### 5.4 What is explicitly **not** needed

| Candidate | Verdict |
| --- | --- |
| **mbedTLS** | **No.** openssl 3.6.0 is already in the image for openssh, libcurl and netsurf, so mbedTLS would be added mass, not a saving. Swapping *everything* to mbedTLS to drop openssl is a real ~4 MB win but it is a separate project — openssh hard-`select`s openssl (`package/openssh/Config.in:4`), so it means dropping Remote Shell too. Out of scope for a 1:1 port; worth its own ticket. |
| **libraqm / harfbuzz** | **No** — and actively harmful. The phone does not have it and must not get it; see 3.9. |
| **libjpeg (IJG)** | No. `BR2_PACKAGE_JPEG_SIMD_SUPPORT` defaults `jpeg-turbo` on for cortex-a7 (NEON) and aarch64, which is what is built today. Keep jpeg-turbo. |
| **A C unit-test framework package** | No target package needed. Host unit tests link against the host build. |
| **libpng for *encoding*** | Only `nd-shoot` writes PNGs, and that is a host-side tool. Build the PNG writer host-only, or write PPM (P6) from the device build and convert on the host. The digest is over raw RGB, so the on-device tool never needs an encoder at all. |

### 5.5 Host-side Python stays

Nothing in this port removes Python from the *build machine*. `mkinitramfs.py`,
`mkupdate.py`, `mkbadupdate.py`, `mkt9dict.py`, `mkicons.py`, `goldenframe.py`,
`shoot_docs.py`, `uistub.py` and the whole pytest suite are host tools and must keep
working. `mkinitramfs.py` in particular is deliberately Pillow-free (`bmp_to_xrgb8888` is
hand-rolled) because nothing guarantees PIL on a build host — keep it that way.

### 5.6 Size and RAM — estimates, and how to replace them with measurements

I could not build here (no `cpio`, `mksquashfs`, `veritysetup`, and no prior
`buildroot/output`), so these are engineering estimates. **Measure before quoting them.**

Authoritative measurement, once a tree is built:

```sh
cd buildroot
make graph-size          # writes output/graphs/{graph-size.pdf,
                         #   file-size-stats.csv, package-size-stats.csv}
```

Capture `package-size-stats.csv` **before** removing Python and again after; the diff is
the real number.

Known hard datum from `docs/PARTITIONS.md`: the squashfs is **49.9 MiB** and the
verity-appended `system.img` is **52.25 MiB**, inside a 100 MiB `rootfs` UBI volume.

Estimated uncompressed target-tree removal:

| Removed | Estimate | Basis |
| --- | --- | --- |
| `/usr/lib/python3.13` (PYC_ONLY, no tests, `unicodedata` on — Kconfig calls it "large") | 10–13 MB | Buildroot strips `.py`, `__pycache__` and the `config-*` dir; the host's 3.11 lib dir is 55 MB with everything |
| `libpython3.13.so.1.0`, stripped, ARM32 | 3.5–4.5 MB | host x86-64 `libpython3.10.so.1.0` is 5.85 MB unstripped |
| Pillow (`_imaging*.so` + PIL `.pyc`) | 2.5–3.5 MB | host PIL package dir is 7.1 MB |
| mutagen `.pyc` | 1–1.5 MB | pure Python |
| python-miniaudio + python-cffi + libffi | 2–3 MB | `_miniaudio` embeds the whole miniaudio decoder set |
| expat, xz (if droppable) | 0.5–0.8 MB | |
| **Total uncompressed** | **≈ 20–26 MB** | |

`.pyc` compresses roughly 2.5–3:1 under zstd/xz, so expect **≈ 7–9 MiB off the 49.9 MiB
squashfs**, i.e. landing near **41–43 MiB**.

Estimated additions: `libneodct.so` + `nd-core` + `nd-apprun` + ~24 `app.so` at `-Os`,
call it 1–2 MB uncompressed; libpng ~200 KB; sqlite (already present via python) 1.2 MB
now charged to us instead. **Net ≈ −6 to −8 MiB compressed.**

RAM — the number that actually motivates the project. `ARCHITECTURE.md`'s own headless
measurement: interpreter 8.4 MB + Pillow 9.0 MB + sqlite 2.3 MB + NeoDCT itself 1–2 MB
≈ 20.8 MB idle on a desktop, 15–17 MB of the phone's 33 MB total. **Every one of those
except sqlite and the ~1–2 MB of real data is runtime overhead this port deletes**, and
the actual picture on screen is 240×175×3 = **126,000 bytes**.

Two config-only wins worth doing alongside, neither requiring any rewriting:

- **`BR2_OPTIMIZE_S`** instead of the default `-O2`, image-wide.
- **musl or uClibc-ng** instead of the default glibc. glibc's own mapping measured 1.5 MB
  and its allocator retains more. This is a defconfig change, not code — but it is a
  *toolchain* change, so it rebuilds everything and can break packages. Do it on its own
  branch, after the port, with the golden oracle already green.

---

## Proposed C modules

### 6.1 Where the package lives — br2-external vs in-tree

Buildroot supports both. The trade-off here is unusual because **Buildroot is vendored
and fully tracked in this repo** (15,026 files under `buildroot/`), so an in-tree package
*is* version-controlled and *does* survive a clone — the normal argument against in-tree
does not apply.

| | in-tree `buildroot/package/neodct/` | br2-external `neodct/br2/` |
| --- | --- | --- |
| Files to add | 3 (+1 line in `package/Config.in`) | 5 (`external.desc`, `Config.in`, `external.mk`, package dir) |
| Build command | unchanged: `make neodct_qemu_defconfig && make` | **changes** to `make BR2_EXTERNAL=… …`, breaking `AGENTS.md`, the README and muscle memory |
| Upstream Buildroot bump | conflicts on `package/Config.in`, one line, trivial to resolve | zero conflicts |
| Matches how this repo already works | yes — the `make update` target is already a 14-line patch to upstream `Makefile` | no |

**Recommendation: in-tree.** The repo has already accepted one upstream modification
(`Makefile` lines 830–843) and gains nothing from the ceremony of a br2-external tree
while keeping the build command unchanged. Record the one-line `package/Config.in`
addition in the same place the `Makefile` patch is recorded, so a future Buildroot bump
knows to reapply both.

If the project would rather keep upstream pristine, the br2-external layout is in 6.3.

### 6.2 In-tree package — exactly what to create

**`buildroot/package/neodct/Config.in`**

```kconfig
config BR2_PACKAGE_NEODCT
	bool "neodct"
	depends on BR2_USE_MMU              # fork()/execve() for nd-apprun
	depends on !BR2_STATIC_LIBS         # dlopen() for app.so
	depends on BR2_TOOLCHAIN_HAS_THREADS
	select BR2_PACKAGE_FREETYPE
	select BR2_PACKAGE_JPEG
	select BR2_PACKAGE_LIBPNG
	select BR2_PACKAGE_SQLITE
	select BR2_PACKAGE_ZLIB
	select BR2_PACKAGE_OPENSSL
	help
	  The NeoDCT phone OS: core process, UI framework, drawing
	  primitives and the shipped apps.

	  https://github.com/<owner>/neodct

comment "neodct needs a toolchain w/ threads, dynamic library"
	depends on BR2_USE_MMU
	depends on !BR2_TOOLCHAIN_HAS_THREADS || BR2_STATIC_LIBS
```

**`buildroot/package/neodct/neodct.mk`**

```make
################################################################################
#
# neodct
#
################################################################################

# Built from the working tree, not a tarball: this is the project's own source.
# SITE_METHOD=local makes buildroot set NEODCT_OVERRIDE_SRCDIR and rsync the
# tree in on every build, so `make neodct-rebuild` is the whole edit loop.
NEODCT_VERSION = custom
NEODCT_SITE = $(TOPDIR)/../neodct/src
NEODCT_SITE_METHOD = local
NEODCT_LICENSE = GPL-3.0
NEODCT_LICENSE_FILES = ../../LICENSE

NEODCT_DEPENDENCIES = host-pkgconf freetype jpeg libpng sqlite zlib openssl

# -Wconversion is in CODING-STANDARDS.md because implicit narrowing on 32-bit
# ARM is a real source of pixel-offset bugs that do not reproduce on a desktop.
NEODCT_MAKE_ENV = \
	$(TARGET_CONFIGURE_OPTS) \
	PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
	NEODCT_CFLAGS="$(TARGET_CFLAGS) -std=c11 -fPIC \
		-Wall -Wextra -Werror -Wshadow -Wconversion \
		-Wstrict-prototypes -Wmissing-prototypes -Wvla \
		-ffunction-sections -fdata-sections" \
	NEODCT_LDFLAGS="$(TARGET_LDFLAGS) -Wl,--gc-sections"

define NEODCT_BUILD_CMDS
	$(NEODCT_MAKE_ENV) $(MAKE) -C $(@D)
endef

define NEODCT_INSTALL_TARGET_CMDS
	$(NEODCT_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
```

**`buildroot/package/neodct/neodct.hash`** — not needed with `SITE_METHOD = local` (no
download happens), but create an empty one with a comment so nobody adds a bogus hash.

**One line into `buildroot/package/Config.in`**, in the "Graphic libraries and
applications (graphic/text)" menu that starts at line 305, alphabetically among the
graphic applications:

```
	source "package/neodct/Config.in"
```

**Both** `neodct/configs/*defconfig` and `buildroot/configs/*defconfig` gain
`BR2_PACKAGE_NEODCT=y`. Verify with `diff` before committing (Risk R-1).

### 6.3 br2-external layout, if that route is preferred instead

```
neodct/br2/
  external.desc          name: NEODCT
                         desc: NeoDCT phone OS
  Config.in              source "$BR2_EXTERNAL_NEODCT_PATH/package/neodct/Config.in"
  external.mk            include $(sort $(wildcard $(BR2_EXTERNAL_NEODCT_PATH)/package/*/*.mk))
  configs/               the two defconfigs move here, and the buildroot/configs copies go away
  package/neodct/        Config.in, neodct.mk   (as 6.2, but SITE = $(BR2_EXTERNAL_NEODCT_PATH)/../src)
```

Build becomes `make BR2_EXTERNAL=$PWD/../neodct/br2 neodct_qemu_defconfig && make`
(the variable is remembered in `output/.br2-external.mk` after the first invocation, so
only the defconfig step needs it). This is the only layout that also **solves the
two-copies-of-every-defconfig problem** properly, by making `neodct/br2/configs/` the
single home. That is a genuine point in its favour.

### 6.4 The C source tree

```
neodct/src/                        NEODCT_SITE points here
  Makefile                         plain make; no autotools, no cmake
  include/nd/*.h                   public headers for libneodct.so
  lib/                             -> libneodct.so   (UI framework, rasterizer,
                                      settings/storage; mapped by core and every app)
  core/                            -> /NeoDCT/System/bin/nd-core
  apprun/                          -> /NeoDCT/System/bin/nd-apprun
  apps/<AppName>/                  -> /NeoDCT/System/apps/<AppName>/app.so
  tools/
    nd_shoot.c                     -> host-only: the golden-frame capture tool (4.1)
    nd_selftest.c                  -> host-only: the pytest bridge (6.6)
  test/
    unit/                          host unit tests, one per module
```

Install layout on the target — keep the current absolute paths, they are load-bearing:

| Built artefact | Installed at |
| --- | --- |
| `libneodct.so` | `/NeoDCT/System/lib/libneodct.so` |
| `nd-core` | `/NeoDCT/System/bin/nd-core` |
| `nd-apprun` | `/NeoDCT/System/bin/nd-apprun` |
| `app.so` | `/NeoDCT/System/apps/<AppName>/app.so`, beside the existing `manifest.json` and `icon.png` |
| `neodct_displayd` | **unchanged** at `/NeoDCT/System/hw/neodct_displayd` — `S90display` and `mkinitramfs.PANEL_DAEMON` both hardcode it |

`run_neodct.sh` changes only in its final section: `python3 /NeoDCT/launcher.py` becomes
`/NeoDCT/System/bin/nd-core`, and `PYTHONPYCACHEPREFIX` goes away. Everything before that
(`dmesg -n 1`, `stty -F /dev/tty0 -echo -tostop`, hiding the cursor, `clear > /dev/tty0`)
and the crash-screen block afterwards stay exactly as they are.

Because `/NeoDCT/System/lib` is not on the default loader path, either add
`-Wl,-rpath,/NeoDCT/System/lib` at link time or ship `/etc/ld.so.conf.d/neodct.conf`.
**Prefer the rpath** — the rootfs is a read-only squashfs and `ldconfig` cannot rebuild
its cache at runtime, which is precisely the class of problem `mkinitramfs.py` already
works around with its `lib64 -> lib` symlinks.

### 6.5 Building `neodct_displayd` from the package

Today `neodctDisplay.c` sits in the overlay and its **compiled ARM binary is committed
next to it** (`ELF 32-bit LSB pie, ARM EABI5, not stripped`, 24,916 bytes). That is a
latent footgun: `mkinitramfs.py` already has to guard against it being the wrong
architecture (`elf_machine(panel) == elf_machine(busybox)`), and nothing in the repo
records how it was built.

The `neodct` package should build it and install it to the same path, and the committed
binary should be deleted. That closes the architecture guard's failure mode and makes the
one existing C program build like the rest.

### 6.6 `nd-selftest` — how the pure-logic pytest files keep working

A large slice of the suite tests Python module behaviour that has no file or subprocess
boundary: T9 (70 tests), storage and settings (38), the update manifest and staging
logic, the clock service. Those cannot import a `.so`.

The cheapest bridge is a single host binary that exposes the ported logic as a
line-oriented command protocol on stdin/stdout:

```
$ nd-selftest t9.candidates 228
cat
bat
act
$ nd-selftest settings.get system.ui.wallpaper
/NeoDCT/System/wallpapers/Palestine.jpg
```

Then the existing test files change from `import System.hw.t9_engine` to a thin
`selftest("t9.candidates", "228")` helper and **keep every assertion, every edge case and
every comment explaining why that edge case exists**. Those comments are the most
valuable thing in the suite and rewriting the tests from scratch throws them away.

This is much better value than porting 659 assertions into a C unit-test framework. Do
both where it is cheap — a C module should still have its own fast unit test — but the
pytest suite is the regression net that already knows about every bug this project has
fixed, and it should keep running against the C.


### 6.7 The oracle, end to end

```
                          neodct/tests/golden/          (committed, recaptured per 3.9)
                                   ▲
   Python build ── goldenframe.py --out ──┘
                                   │
                                   ├── goldenframe.py --compare golden/ frames/
                                   │            ├─ identical  → exit 0
                                   ▼            └─ differs    → per-frame "N px (P%) in box (l,t,r,b)"
   C build ────── nd-shoot --out frames/ ───────┘
```

Four rungs, in the order they should be built. Each is cheap and each catches a class of
bug the next one cannot.

**Rung 1 — determinism gate.** Before trusting anything: `goldenframe.py
--verify-determinism /tmp/det` captures twice and compares. Confirmed working here:
5.7 s per capture, `identical: 49 frames match`. If this ever fails, a new source of
nondeterminism has crept into the Python and the reference is worthless until it is
found.

**Rung 2 — exact pixel equality.** `nd-shoot` vs `golden/`. This is the real oracle and
the thing "one-to-one" means. Expect it to be red for a long time and to go green
screen by screen.

**Rung 3 — structural properties, for while rung 2 is red.** `test_update_ui.py` already
does exactly this and is the model: 32 tests that assert geometric invariants on real
frames — nothing overlaps the progress bar, nothing spills into the softkey bar, release
notes scroll rather than overflow. Its helpers are twelve lines:

```python
def lit_pixels(image, box):
    x0, y0, x1, y1 = box
    pixels = image.load()
    return [(x, y) for y in range(max(0, y0), min(image.height, y1))
                   for x in range(max(0, x0), min(image.width, x1))
                   if pixels[x, y] != (0, 0, 0)]

def is_clear(image, box):
    return not lit_pixels(image, box)
```

Point them at `frames/` and they judge the C build with no changes at all. This is the
rung that says "the C port is *usable*" long before it says "the C port is *identical*",
and it lets a wave of app agents make progress without one antialiasing pixel blocking
everyone.

**Rung 4 — text presence.** A frame that renders the right layout with the wrong string
passes rung 3. `nd-shoot --dump-text` writing one line per `nd_draw_text` call
(`x y size "string"`) gives a trivially diffable trace, and catches wrong strings, wrong
fonts and wrong positions long before the pixels are close. `test_systemupdate_app.py`'s
`Recorder.page_text()` is the same idea at widget level.

**Suggested CI, which does not exist yet:**

```yaml
# .github/workflows/test.yml  -- there is no test workflow today
on: [push, pull_request]
jobs:
  pytest:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get install -y cpio openssh-client openssh-server figlet
      - run: pip install pillow
      - run: python3 -m pytest neodct/tests/ -q
  golden:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: pip install pillow
      # 5.7 s per capture. Both gates on every push.
      - run: python3 neodct/tools/goldenframe.py --verify-determinism /tmp/det
      - run: python3 neodct/tools/goldenframe.py --out /tmp/py
      - run: python3 neodct/tools/goldenframe.py --compare neodct/tests/golden /tmp/py
```

The `golden` job protects the reference from drifting under the C team's feet: if
somebody changes the Python UI without recapturing, CI says so immediately instead of the
C port mysteriously going red weeks later. Add a third job building `nd-shoot` and
running rung 2 as soon as the first C screen renders.

**Note the apt list.** Reproducing a green suite needs `cpio` (mkinitramfs), `openssh`
(RemoteShell tests) and `figlet` (banner). Missing them is exactly the 17 failures seen
in this sandbox — environmental, not regressions, and worth pinning in CI so nobody
spends an afternoon chasing them.

---

## Risks

| # | Risk | Severity | Mitigation |
| --- | --- | --- | --- |
| R-1 | **The golden reference is not device-accurate.** Captured with Raqm layout; the phone uses BASIC. **46 of 49 frames change**, up to 16.81%. A C port matched against today's references would be matched against the wrong thing. | **high** | Pin `ImageFont.core.HAVE_RAQM = False` in `_Frozen.__enter__`, recapture, and assert in `capture()` that raqm is off. Do this **before any C is written**. Section 3.9 has the patch. |
| R-2 | **Two copies of every defconfig** (`buildroot/configs/`, `neodct/configs/`). Byte-identical today. The build uses the `buildroot/` copy, so editing only the other one silently builds the old image. | medium | Edit both in the same commit; `diff` before committing. Better: a pytest that fails when they differ (nine lines, and there is no such test today). Best: move to br2-external (6.3), which removes the duplication by design. |
| R-3 | **FreeType version skew.** Host 2.14.3, Buildroot 2.14.1. Hinting or rasterizer changes between point releases would shift antialiasing coverage values and break exact equality between a host-built C `nd-shoot` and the phone. Unquantified — I could not build 2.14.1 here. | medium | Record the FreeType version in the manifest and have `compare()` warn on mismatch. Consider bumping Buildroot's freetype to match the host, or pinning the host to 2.14.1, so both sides use one version. Cheapest real test: run `nd-shoot` on the device over the serial console and compare against the host capture. |
| R-4 | **LANCZOS resampling is untested.** `Palestine.jpg` is already 240×175, so `Image.resize()` short-circuits and LANCZOS never runs in any golden frame — yet three stock wallpapers are 240×240 and two are 240×170 and all five do run it. A wrong C LANCZOS passes the oracle. | medium | Add `home-wallpaper-<slug>` shots for at least one 240×240 and one 240×170 wallpaper. Five lines in `shoot_docs.shoot_home`, and it turns an invisible hazard into a checked one. |
| R-5 | **JPEG decode must match bit-for-bit.** Host libjpeg-turbo 3.1.4.1 vs Buildroot's 3.1.2, and the SIMD path differs by architecture. Baseline JPEG's IDCT is specified but not bit-exact across implementations, and 30 of 49 goldens are wallpapered. | medium | Link `nd-shoot`'s host build against the *same* libjpeg the reference capture used. Longer term, decode the six stock wallpapers once at build time into a raw blob so the runtime never decodes JPEG at all — that also removes libjpeg-turbo from the target and saves RAM. Raise as an open question first: it is a behaviour change (user-supplied wallpapers still need a decoder). |
| R-6 | `compare()` **ignores `epoch`, `tick` and `seed`** in the manifests. Two directories captured with different constants compare as "identical" if the pixels happen to match, and as an unexplained diff if they do not. | low | Six lines in `compare()`: read all three from both manifests and fail loudly on mismatch. |
| R-7 | **Python's `random` reaches pixels** in Snake (food placement) and Memory (card shuffle). Reproducing CPython's MT19937 *and* the exact consumption pattern of `randint`/`shuffle` in C is doable but fragile — a single extra `random()` call anywhere shifts the sequence. | medium | Do not reimplement MT19937. Give both games an injectable random source and pin it in both harnesses (e.g. a fixed 64-entry table, or a tiny shared xorshift). Two frames are affected (`game-snake`, `game-memory`) and they are worth two hours, not two weeks. Raise as an open question — it is a deliberate deviation. |
| R-8 | **`BR2_PACKAGE_SQLITE` is only reachable through `BR2_PACKAGE_PYTHON3_SQLITE`.** Deleting the Python lines without adding it produces an image that builds, boots, and has no phonebook, no messages and no call log. | **high** | Add `BR2_PACKAGE_SQLITE=y` in the **same commit** that removes `BR2_PACKAGE_PYTHON3=y`, never after. Same for `OPENSSL` and `ZLIB`, which survive today only because openssh selects them. |
| R-9 | **The committed `neodct_displayd` binary.** Prebuilt, committed, not stripped, unknown provenance, present even in target trees of the wrong architecture — `mkinitramfs.py` has a guard specifically for that. | medium | Build it from the `neodct` package (6.5) and delete the committed binary. Keep the architecture guard regardless. |
| R-10 | **No CI runs the tests.** The only workflow is `release.yml`, which checks a tag against `VERSION_ID` and builds release notes. Nothing enforces that 659 tests pass or that the golden set still matches. | **high** | Add the workflow in 6.7. It is the cheapest high-value change in this whole subsystem and it should land before the port starts, not after. |
| R-11 | **`luckfox_pico_mini_defconfig` builds its kernel from `board/qemu/aarch64-virt/linux.config`** while declaring `BR2_arm` + `BR2_cortex_a7`. Either it is dead (the real kernel comes from the Rockchip SDK, as `AGENTS.md` implies) or it is wrong. | low | Pre-existing, not caused by the port. Confirm with the owner and either delete the kernel lines from that defconfig or point them at a real ARM config. Raise as an open question. |
| R-12 | **Two different rounding conventions.** Text compositing rounds (`+127`, 3.8.1); wallpaper dimming truncates (3.8.2). An agent who "unifies" them breaks 30 frames. | medium | Both formulas are in this document with their measured evidence and mismatch counts. Put both in the rasterizer's header comment with a note that they deliberately differ. |
| R-13 | **`device_frame()` centres at y=32; the hardware bottom-aligns at y=65.** `home-panel` and `menu-panel` are documentation aids, not device output. | low | Documented in 3.7. `nd-shoot` must reproduce y=32 for those two names. Consider marking them informational in a future manifest revision. |
| R-14 | **`shoot_docs.py` swallows per-shot exceptions** (`except BaseException: print("  !! …"); continue`). A screen that fails to render vanishes from the manifest and shows up only as `missing` in a later compare. `shoot_examples` currently produces nothing at all this way. | low | `nd-shoot` must exit non-zero on a missing frame. Consider making `capture()` fail too, or at least assert the frame count is 49. |
| R-15 | **`AGENTS.md` says "510 tests"** — it is 659 functions / 676 collected items. Small, but agents calibrate on it. | low | One-line fix in `AGENTS.md` while the port is in flight. |
| R-16 | **`.clang-format` does not exist**, though `CODING-STANDARDS.md` says to run it "with the repo's `.clang-format`". Ten agents will produce ten formattings. | low | Add one before any C lands: 4 spaces, no tabs, 100-column limit, braces on the same line for control flow and on their own line for function bodies. |
| R-17 | **The test suite is not hermetic.** `test_mkinitramfs.py` reads the host's real `/usr/bin/ls` and `/usr/lib`, so it fails on a host whose libraries live outside its four search dirs (11 failures here). Others need `sshd`, `ssh-keygen`, `cpio`, `figlet`. | low | Pin the toolchain in the CI job (6.7). Do not "fix" the tests to be hermetic — reading real ELF files is the point of them. |

---

## Tests that cover this

### 8.1 The suite as it stands

36 files, 9,399 lines, **659 `def test_` functions**, 676 collected items.
Measured here: **657 passed, 17 failed, 2 skipped in 21.9 s** — all 17 failures
environmental (section 8.5).

### 8.2 Directly covering this subsystem

| File | LOC | Tests | What it pins | Survives the port? |
| --- | --- | --- | --- | --- |
| `test_uistub.py` | 446 | 37 | Every part of the harness the oracle rests on: frames are copied not referenced; `device_frame` centres at 240×240 with the band starting at **row 32**; `PathRemap` maps `/NeoDCT/...` and restores `builtins.open` on exit; `KeyScript` ordering, `on_exhausted="raise"`, `idle_budget` reset semantics; `StubUI` scans manifests, sorts by id with `ids[0] == 1`, renders 240×175, loads the pixel font at **size 24**, applies a wallpaper to 240×175, hides engineering apps when `engineering=False`, **never opens a real `/dev/input` node**, never writes into the repo overlay; `run_app` frame budget and its non-leakage into later screens. | **Yes, unchanged** — it tests the Python reference side, which stays. |
| `test_mkinitramfs.py` | 341 | 17 | ELF `DT_NEEDED` reading, dependency closure, `MissingLibrary` on an unresolvable lib, cpio contents, `init` executable, dmsetup discovered where Buildroot installs it, loader aliases present and surviving into the archive, splash byte order and top-down emission, wrong-size splash fails the build, the panel daemon shipped only on matching architecture, the BMP itself excluded. | **Yes, unchanged.** |
| `test_initramfs_applets.py` | 124 | 3 (parametrized per script) | Statically scans `neodct/initramfs/*` for command words and fails if `mkinitramfs.APPLETS` lacks one. Knows ash builtins and `dmsetup`. | **Yes, unchanged.** |
| `test_post_build_metadata.py` | 157 | 8 | The platform is the **last** argument, not `$2`; anything containing `/` becomes `unknown`; version comes from `VERSION_ID`; `buildepoch` and `buildtime` both recorded; `NeoDCT/User` created; no `VERSION_ID` ⇒ non-zero exit with `VERSION_ID` on stderr. Plus two repo-consistency checks: `os-release` agrees with itself across `VERSION`/`VERSION_ID`/`PRETTY_NAME`, and `CHANGELOG.txt` has a section headed exactly that version. | **Yes, unchanged.** |
| `test_post_build_prune.py` | 92 | 5 | A deleted app is dropped, surviving apps are kept, engineering apps pruned the same way, the removal is announced on stdout, a target with no apps directory is not an error. | **Yes, unchanged** — and *more* important with `app.so`. |
| `test_sdcard_helper.py` | 315 | 25 | The `neodct-sdcard` shell helper via subprocess. | Yes, unchanged. |
| `test_mkupdate.py` / `test_mkbadupdate.py` / `test_mkt9dict.py` | 270/235/161 | 17/17/11 | Host packaging tools. | Yes, though `mkupdate` and `mkt9dict` import `System.core.UpdateService` / `System.hw`, so they pick up whatever those become. |

**Total surviving unchanged: ≈ 123 tests.**

### 8.3 Can they act as a port oracle?

Yes — three of the four rungs already exist in the repo, which is unusual and valuable.

| Mechanism | Where it already lives | Reusable against C? |
| --- | --- | --- |
| Exact pixel equality | `goldenframe.py` + `neodct/tests/golden/` (49 frames) | **Yes, directly.** `--compare` is build-agnostic. Needs the 3.9 fix first. |
| Structural pixel properties | `test_update_ui.py` (32 tests) and its `lit_pixels`/`is_clear` helpers | **Yes** — point them at the C frame dump. |
| Drawn-text trace | `test_systemupdate_app.py`'s `Recorder.page_text()` (widget level) | Concept yes, code no — needs `nd-shoot --dump-text` (6.7 rung 4). |
| Pure-logic assertions | 70 T9 tests, 38 storage/settings, ~150 update-service | Via `nd-selftest` (6.6), keeping every assertion and comment. |

### 8.4 Coverage gaps in the reference set

The 49 frames cover 11 of 13 stock apps and 5 of 11 engineering apps. Not covered at all:

- **Apps:** `Browser` (out of scope — netsurf), **`Update`**, **`Power`**.
- **Engineering:** `Crash`, `Downgrade`, `KeypadMapper`, `KeypadMapperI2C`, `LinuxShell`,
  `RemoteShell`.
- **Framework paths:** T9 predictive entry (`test_framework_predictive.py` has 25 tests
  but no golden frame), the incoming-SMS *modal*, the first-boot security notice
  (`skip_notice=True` suppresses it in every shot), LANCZOS wallpaper scaling (R-4),
  every wallpaper except `Palestine.jpg`.
- **The five `example-*` shots** the code can produce but the tutorial apps for which are
  absent from this checkout.

Recommended additions, all cheap, all in `shoot_docs.py`:

| New frame | Why |
| --- | --- |
| `home-wallpaper-90s-throwback` (240×240 source) | exercises LANCZOS downscale (R-4) |
| `home-wallpaper-grasslands` (240×170 source) | exercises LANCZOS upscale |
| `app-update-card`, `app-update-notes` | the Update app is unshot and `update_ui_fixtures.py` already builds everything needed |
| `widget-textinput-t9` | 25 predictive tests, zero pixels |
| `boot-security-notice` | `StubUI(skip_notice=False)` |
| `app-power` | one unshot stock app |

Each is 3–6 lines and each closes a hole where a wrong C implementation would pass.

### 8.5 The 17 failures seen here, itemised

Not regressions. Reproduce a clean run by installing `cpio`, `openssh-client`,
`openssh-server` and `figlet`.

| Count | Failure | Cause |
| --- | --- | --- |
| 11 | `test_mkinitramfs.py::*` — `MissingLibrary: libselinux.so.1 not found in /usr/lib, /lib, /lib64, /usr/lib64` | This host's `/usr/bin/ls` needs libselinux, which lives in a multiarch dir outside `mkinitramfs.DEFAULT_LIB_DIRS`. The tests deliberately read host binaries. |
| 5 | `test_remoteshell.py::*` — "This build has no ssh server." | No `sshd`/`ssh-keygen` on PATH. |
| 1 | `test_systemupdate_app.py::test_a_backup_that_failed_is_admitted_before_the_restart` | Assertion on rendered page text; worth a second look but unrelated to this subsystem. |

Also absent here and needed for a full build: `cpio`, `mksquashfs`, `veritysetup`,
`figlet`.

---

## How this could be split across agents

This subsystem is **not** a good candidate for wide parallelism — it is small, and its
value is that everyone else depends on it. But it is a good candidate for being done
**first**, quickly, by one or two agents, before the rest of the port starts.

### Phase 0 — before any C is written (blocking, ~1 day, one agent)

| Task | Depends on | Notes |
| --- | --- | --- |
| **0a. Pin the layout engine and recapture the golden set** | nothing | Section 3.9. Five lines in `goldenframe.py`, one recapture, 49 new PNGs committed. **Everything else in the port is measured against this.** Nothing may start before it lands. |
| 0b. Add the test CI workflow | nothing | Section 6.7. Also protects the reference from drifting. |
| 0c. Harden `compare()` — check `epoch`/`tick`/`seed`, record and warn on FreeType version | 0a | R-3, R-6. |
| 0d. Add the coverage-gap frames | 0a | Section 8.4. Recapture again; do it while the reference is already being rewritten, not after. |
| 0e. Add `.clang-format`; fix "510 tests" in `AGENTS.md`; add the defconfig-parity test | nothing | R-15, R-16, R-2. Fifteen minutes total. |

Phase 0 is one agent's day and it removes the largest risk in the entire project.

### Phase 1 — build plumbing (parallel with the foundation work, one agent)

| Task | Depends on | Notes |
| --- | --- | --- |
| 1a. Create `buildroot/package/neodct/{Config.in,neodct.mk}` + the `package/Config.in` line, with a stub `neodct/src/` that builds an empty `libneodct.so` and a `nd-core` that prints and exits | nothing | Section 6.2. Prove `make neodct-rebuild` works end to end before anyone writes real code. |
| 1b. Add `BR2_PACKAGE_NEODCT=y` to all **four** defconfig copies | 1a | R-1. |
| 1c. Move `neodctDisplay.c` into the package; delete the committed binary | 1a | R-9. |
| 1d. `nd-shoot` skeleton: `--list` prints the 49 names, `--out` writes 49 all-black PNGs plus a schema-correct `manifest.json` | 1a | Makes the oracle wiring testable before a single pixel is right. `compare()` should then report exactly 49 `pixels` diffs and no `missing` — that is the plumbing working. |
| 1e. `ND_ROOT` / `nd_path_resolve()` in `libneodct.so` | 1a | The `PathRemap` equivalent. Introduce it now; retrofitting it later across every file open is misery. |

### Phase 2 — after the rasterizer exists (one agent, alongside the UI framework agent)

| Task | Depends on | Notes |
| --- | --- | --- |
| 2a. Virtual clock + PRNG pinning inside `nd-shoot` | 1d, rasterizer | Section 3.5.4/3.5.5. |
| 2b. Wire rung 3: point `test_update_ui.py`'s helpers at a C frame dump | 1d | Gives the app agents a usable green signal while rung 2 is still red. |
| 2c. `nd-shoot --dump-text` | rasterizer | Rung 4. |
| 2d. `nd-selftest` protocol + convert the T9 tests as the pilot | core logic ports | 70 tests is a good first slice and T9 is self-contained. |

### Phase 3 — during the app waves (no dedicated agent)

Each app agent adds its own shot to `nd-shoot` and drives its own frame green. That is
the natural unit of parallelism and it needs no coordination beyond the frame names,
which this document fixes.

### What must **not** be parallelised

- **The golden recapture (0a).** Two agents recapturing concurrently with different
  settings produces a reference nobody can trust. One agent, one commit.
- **Defconfig edits.** Four files, two of which must stay byte-identical to the other
  two. One agent owns all defconfig changes for the duration of the port.
- **`package/Config.in`.** One line, but it is upstream Buildroot and every concurrent
  edit is a conflict.

### Open questions raised by this survey

To be added to `docs/c-rewrite/OPEN-QUESTIONS.md`:

1. **`game-snake` / `game-memory` randomness** — reproduce CPython's MT19937 exactly, or
   give both games an injectable random source pinned identically in both harnesses?
   (R-7; the second is a deliberate deviation.)
2. **JPEG wallpapers** — decode at build time into raw blobs so the target never links
   libjpeg, or keep runtime decode and accept the cross-version bit-exactness risk?
   (R-5; the first is a behaviour change for user-supplied wallpapers.)
3. **`luckfox_pico_mini_defconfig` kernel lines** — dead, or wrong? They declare
   `BR2_arm`/`BR2_cortex_a7` and then build from `board/qemu/aarch64-virt/linux.config`.
   (R-11.)
4. **br2-external** — worth changing the documented build command to get one home for the
   defconfigs, or keep the in-tree package and live with two copies? (6.1/6.3, R-2.)

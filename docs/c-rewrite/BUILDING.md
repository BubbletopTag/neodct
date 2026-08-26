# Building NeoDCT 0.4.0

Two builds, and you want both. The **host build** takes seconds and is where you
work; the **Buildroot build** takes hours and produces the thing that boots.

---

## 0. Getting the branch

The C rewrite lives on **`c-rewrite`**. `main` is still the Python.

If you already have the repository:

```sh
git fetch origin c-rewrite
git checkout c-rewrite
```

From scratch:

```sh
git clone https://github.com/BubbletopTag/neodct.git
cd neodct
git checkout c-rewrite
```

The clone is around 51 MB of history, because Buildroot is vendored rather than
being a submodule — 15,000 tracked files. There is nothing to `git submodule
update`; what you clone is what you build.

### What you need before starting

| | Host build | Buildroot build |
| --- | --- | --- |
| Time | seconds | **hours** on a cold tree |
| Disk | ~200 MB | **10–15 GB** |
| Network | none after `apt` | **unrestricted** — see below |
| Gets you | tests, screenshots, `nd-core` on your laptop | an image that boots |

**Buildroot downloads several hundred upstream tarballs.** Any network that
filters outbound HTTPS will fail at the first one with
`Proxy tunneling failed: Forbidden`, before a single package builds. If a
mirror 403s on plain HTTP, fetching the tarball yourself over HTTPS into
`buildroot/dl/<package>/` and re-running `make` is enough — the hash is checked
either way, so a hand-fetched file is no less safe.

### NetSurf

The browser is built from the vendored fork in `netsurf-neodct/`, which is in
the repository — `buildroot/package/netsurf/netsurf.mk:21` rsyncs `netsurf/`
and `libnsfb/` over the extracted upstream tarball.

Verified building from a clean checkout on this branch:

```
/usr/bin/netsurf-fb    3,275,568 bytes
ELF 64-bit, ARM aarch64
interpreter: /lib/ld-musl-aarch64.so.1
```

If you ever see the browser fail with

```
make[2]: *** No rule to make target 'install'.  Stop.
make[2]: Leaving directory '.../netsurf-3.11/libnsfb'
```

that is `netsurf-neodct/libnsfb` having become a **git submodule** again
(`git ls-tree HEAD netsurf-neodct/libnsfb` showing mode `160000` rather than
`040000`). A gitlink with no `.gitmodules` clones as an empty directory, the
rsync copies nothing, and the failure surfaces inside the browser's own
makefile rather than anywhere that names the cause. It also will not reproduce
on the machine that created the fork, because a real checkout is sitting there.

To build without the browser — useful when bisecting something unrelated, since
NetSurf is one of the longest packages in the tree:

```sh
cd buildroot
make neodct_qemu_defconfig
sed -i 's/^BR2_PACKAGE_NETSURF=y/# BR2_PACKAGE_NETSURF is not set/' .config
make olddefconfig
make
```

That edits `.config` only, never the tracked defconfigs. The Browser app's
launcher is ported and simply finds no `netsurf-fb` to launch.

**One thing worth not growing:** `netsurf.mk` reads only `netsurf/` and
`libnsfb/`. `tests/` is 52 MB of the 74 MB and the build never touches it;
`prefix/` is build output (`include/`, `lib/`). Both are already permanent in
history.

---

## 1. The host build — start here

Compiles the OS for your laptop, runs 40,000 tests, and renders the phone's
screens to PNG files. No Buildroot, no cross-compiler, no QEMU. Seconds, not
hours.

### Dependencies (Ubuntu 24.04)

```sh
sudo apt install build-essential pkg-config \
     libfreetype-dev libpng-dev libjpeg-dev libsqlite3-dev \
     musl-tools figlet
```

`musl-tools` gives you `musl-gcc`, which the verification script uses to catch
glibc-only calls at the moment they are written rather than at the first
cross-build. `figlet` renders the green boot banner.

### Build and test

```sh
cd neodct/src
make                 # build everything
make test            # 30+ test binaries, ~40,000 assertions
make ASAN=1 test     # again under AddressSanitizer + UBSan
```

`make ASAN=1 test` is the one that matters. C's worst failures — use-after-free,
buffer overruns, uninitialised reads — pass ordinary tests and surface weeks
later on the device. AddressSanitizer turns them into a stack trace at the
moment they happen. Run it before every commit.

### The full gate

```sh
./neodct/tools/verify-c-build.sh
```

Eight checks: clean compile with `-Werror`, the warning flags are *actually
still in the Makefile*, every source compiles under musl, tests pass, tests pass
under sanitizers, rendered frames match the Python pixel-for-pixel, all 380
glyph records match, and idle memory is under budget. Exit status is the failure
count, so it drops into CI unchanged.

### Look at the screens

```sh
neodct/src/build/default/bin/nd-shoot --out /tmp/frames
python3 neodct/tools/goldenframe.py --compare neodct/tests/golden /tmp/frames
```

`nd-shoot` renders the phone's screens headlessly and writes PNGs. The compare
prints a differing-pixel count and a bounding box for anything that does not
match, so a mismatch tells you *where*.

**It skips frames it cannot honestly produce** rather than emitting a blank —
so the skip list is the remaining work, not padding. `nd-shoot-skipped.json` in
the output directory says why each one was skipped.

### Run the OS on your laptop

```sh
# stage a root: overlay assets, then the built binaries over the top
rm -rf /tmp/ndroot && mkdir -p /tmp/ndroot
cp -a neodct/overlay/NeoDCT /tmp/ndroot/
find /tmp/ndroot -name '*.py' -delete
make -C neodct/src install DESTDIR=/tmp/ndroot
mkdir -p /tmp/ndroot/NeoDCT/User/{db,logs,wallpapers} /tmp/ndroot/etc
figlet -f small "NeoDCT OS v0.4.0" > /tmp/ndroot/etc/neodct-banner

NEODCT_ROOT=/tmp/ndroot neodct/src/build/default/bin/nd-core --headless --idle-measure
```

`NEODCT_ROOT` redirects every absolute `/NeoDCT/...` path into the staged tree,
which is how the host tests reach the real assets without a real rootfs. The
`figlet` line is what `post-build-system-metadata.sh` does during a real build —
skip it and the boot banner is two green rules with nothing between them.

---

## 2. The Buildroot build — the image that boots

**This needs unrestricted network access.** Buildroot downloads several hundred
upstream tarballs from `sources.buildroot.net`, `ftpmirror.gnu.org`, kernel.org
and others. Any environment that filters outbound HTTPS will fail at the first
download with `Proxy tunneling failed: Forbidden`, before a single package
builds.

### Ubuntu 24.04 distrobox

The vendored Buildroot wants a predictable host. A distrobox keeps it off your
main system:

```sh
distrobox create --name neodct-build --image ubuntu:24.04
distrobox enter neodct-build

sudo apt update && sudo apt install -y \
    build-essential git wget cpio unzip rsync bc file \
    libncurses-dev python3 python3-dev perl \
    qemu-system-arm qemu-system-aarch64
```

Note `AGENTS.md`: the **hardware** kernel builds through the Rockchip SDK and
that specifically needs Ubuntu 22.04 — a native Arch host hangs on the atbm wifi
driver. The QEMU target below is fine on 24.04.

### Build

```sh
cd buildroot
make neodct_qemu_defconfig     # first time only; overwrites .config
make                           # hours on a cold tree
```

Images land in `buildroot/output/images/`.

### Run it

```sh
neodct/tools/run_qemu.sh
```

`run_qemu.sh` is driven entirely by environment variables — `NEODCT_SNAPSHOT`,
`NEODCT_VERITY`, `NEODCT_SD`, `NEODCT_MODEM`, `NEODCT_NET`, `NEODCT_DEBUG` and
more. Read its header before adding a flag; the one you want probably exists.

**Give the VM 72 MB.** That lands around 55 MB of usable memory, which is what
the 32-bit Luckfox actually has after the kernel and CMA take their share. A VM
with more RAM will happily run things the phone cannot.

### Rebuilding after a code change

```sh
cd buildroot
make
```

`SITE_METHOD = local` means the package builds straight from `neodct/src` with
no version bump, no hash and no download. That is the whole edit loop.

**This did not use to be true, and the failure was silent.** Buildroot gates
the copy-from-working-tree step on a stamp file with no prerequisites
(`pkg-generic.mk:222`), so once it existed, a plain `make` walked straight past
this package: you pulled, you built, and you got a byte-identical image with
nothing telling you nothing had been rebuilt. `neodct.mk` now checks the tree's
mtimes itself and drops the stamps when `neodct/src` is newer than the last
build, printing `neodct: source changed, will rebuild`. `make neodct-rebuild`
still works and still forces it; `NEODCT_NO_AUTO_REBUILD=y` turns the check off.

Three kinds of change, three different costs:

| You changed | What to run | Cost |
| --- | --- | --- |
| `neodct/src` (C code) | `make` | a minute |
| `neodct/overlay`, `neodct/scripts` | `make` | seconds — `target-finalize` re-copies the overlay and re-runs the post-build scripts every time |
| a **defconfig** | `make neodct_qemu_defconfig && make` | see below |

The defconfig one is the trap. `make` never regenerates `.config` from the
tracked defconfig, so a pulled defconfig change does nothing at all until you
re-run `make neodct_qemu_defconfig` — which overwrites `.config`, losing any
local `menuconfig` edits. And if the pulled change touched the **toolchain or
the CPU** (`BR2_cortex_a53`, `BR2_TOOLCHAIN_BUILDROOT_MUSL`, the FPU flags),
buildroot cannot rebuild incrementally across it — that is a full tree rebuild,
hours. If you only want the code changes, don't re-run the defconfig.

---

## 3. What changed in 0.4.0

The C build is wired in through three things:

| | |
| --- | --- |
| `buildroot/package/neodct/` | the package — `Config.in`, `neodct.mk`, `neodct.hash` |
| `buildroot/package/Config.in` | one line sourcing it, alphabetically before `netsurf` |
| both defconfigs | `BR2_PACKAGE_NEODCT=y` and `BR2_TOOLCHAIN_BUILDROOT_MUSL=y` |

**Defconfigs live in two places** — `buildroot/configs/` and `neodct/configs/`.
The build uses the `buildroot/` copy. Edit both or the next person builds
something else. `diff` them before committing.

The package selects `BR2_PACKAGE_SQLITE` explicitly. Until Python left, sqlite
was reachable *only* through `BR2_PACKAGE_PYTHON3_SQLITE`, so an image that
dropped Python without that line would have built cleanly and shipped with no
phonebook, no messages and no call log. It also selects freetype, jpeg, libpng,
zlib and openssl for the same reason.

**Python is out of the image.** Every app is real, so `BR2_PACKAGE_PYTHON3`,
Pillow, mutagen and miniaudio have left all four NeoDCT defconfigs, and the
overlay's 1.6 MB of `.py` has moved to `neodct/python-reference/` — it is the
specification the port cites by file and line, and it must not ship. Removing a
package from a defconfig is one of the changes buildroot cannot do
incrementally: `output/target` keeps the files until a clean rebuild, so
`make neodct_qemu_defconfig && make clean && make` is what actually sheds
them.

### musl

0.4.0 switches from glibc to musl. One thing had to change first:
`neodct_displayd` was a committed, prebuilt, glibc-linked ARM binary
(`interpreter /lib/ld-linux-armhf.so.3`). Under musl that loader does not exist
and the kernel cannot exec it at all — and `mkinitramfs.py` copies the same file
into the initramfs, so it would have broken the boot-time update applier too.

It is now built from source (`neodct/src/displayd/`) on every `make` and
installed to the same path, so nothing that references it had to change.
`docs/c-rewrite/MUSL.md` has the details, including the differences that touch
this codebase — `sin`/`cos` disagreeing by an ULP, `malloc_trim` being absent,
and musl's 128 KB default thread stack against glibc's 8 MB.

---

## 4. Where things are

```
neodct/src/
  include/     public headers — the contract every module builds against
  lib/         libneodct.so: drawing, font, widgets, storage, services
  core/        nd-core, the process that owns the screen and never dies
  apprun/      nd-apprun, the stub that dlopens one app in its own process
  apps/        one directory per app -> app.so
  displayd/    the ST7789 SPI panel daemon
  tools/       nd-shoot
  test/unit/   host tests
  test/apps/   apps that exist only to be launched BY a test, including the
               deliberately faulting one that proves the crash screen works
neodct/tests/golden/   49 reference screens captured from the Python build
neodct/tools/          goldenframe.py, fontref.py, logref.py, verify-c-build.sh
```

`neodct/overlay/` still holds everything that is not code — fonts, wallpapers,
ringtones, icons, `manifest.json` files, `/etc`, the initramfs scripts. Those
stay directly editable with no rebuild.

**`neodct/overlay/NeoDCT/` is still the Python, and it is load-bearing.** It is
the reference the C is checked against, and the only way to re-cut a golden
frame. Do not delete it until the port is finished.

It does **not** ship. `post-build-prune-tests.sh` deletes every `.py` from the
target — 74 files, about 2 MB — because nothing on the device runs them:
`nd-core` is the init process and `nd-apprun` dlopen()s `app.so` from a
compile-time constant, so the `"exec": "main.py"` line in every
`manifest.json` is never read by the C at all. It stays in the repository
because the Python reference build does read it. `NEODCT_KEEP_PYTHON=1 make`
keeps them in the image if you ever want to compare the two side by side on
one device.

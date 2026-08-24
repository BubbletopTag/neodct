# Building NeoDCT 0.4.0

Two builds, and you want the first one nearly all the time.

| | what it is | how long |
| --- | --- | --- |
| **Host build** | the OS compiled for your laptop, rendering screens to PNG | ~20 seconds |
| **Buildroot image** | the real bootable phone image | hours, cold |

The host build is the edit loop. Use it for everything except actually booting.

---

## 1. Host build (start here)

### Dependencies — Ubuntu 24.04

```sh
sudo apt install build-essential pkg-config \
                 libfreetype-dev libpng-dev libjpeg-dev libsqlite3-dev \
                 musl-tools python3-pil
```

`musl-tools` and `python3-pil` are only needed to run the full checks:
musl for the libc the phone uses, Pillow for the frame comparison.

### Build and test

```sh
cd neodct/src
make                 # ~20 s
make test            # every unit test
make ASAN=1 test     # again under AddressSanitizer -- do this before pushing
```

### See the screens

```sh
./build/default/bin/nd-shoot --out /tmp/frames
python3 ../tools/goldenframe.py --compare ../tests/golden /tmp/frames
```

`nd-shoot` renders the UI headlessly and writes PNGs. The comparison prints
`identical: N frames match`, or a pixel count and a bounding box for anything
that moved. Frames whose app is not ported yet are listed as *missing*, with
the reason in `/tmp/frames/nd-shoot-skipped.json` — that is remaining work,
not a failure.

### Run it

```sh
NEODCT_ROOT=/tmp/ndroot ./build/default/bin/nd-core --headless --idle-measure
```

`NEODCT_ROOT` redirects the hardcoded `/NeoDCT/...` paths into a staging
directory, the same trick `uistub.py` used for the Python. Stage one with:

```sh
mkdir -p /tmp/ndroot && cp -a neodct/overlay/NeoDCT /tmp/ndroot/
make -C neodct/src install DESTDIR=/tmp/ndroot
mkdir -p /tmp/ndroot/NeoDCT/User/{db,logs,wallpapers}
```

### The full gate

```sh
./neodct/tools/verify-c-build.sh      # from the repo root
```

Eight checks: clean `-Werror` build, the warning flags still present in the
Makefile, musl compilation, unit tests, unit tests under ASan+UBSan, frame
comparison, all 380 glyph records, and idle RSS. Exit status is the failure
count. **Run this before pushing** — it is what CI runs.

---

## 2. Buildroot image

### Why a distrobox

The Rockchip SDK kernel build hangs on the atbm wifi driver on Arch, so kernel
work happens in an Ubuntu 22.04 container (`docs/HARDWARE_NOTES.md`). The
Buildroot build itself is happy on Ubuntu 24.04.

```sh
distrobox create --name neodct --image ubuntu:24.04
distrobox enter neodct

sudo apt update && sudo apt install -y \
    build-essential git wget cpio unzip rsync bc file \
    libncurses-dev python3 python3-dev
```

Buildroot refuses to run as root, so use a normal user inside the box.

### QEMU (the development target)

```sh
cd buildroot
make neodct_qemu_defconfig     # first time only -- overwrites .config
make                           # hours on a cold tree
cd .. && neodct/tools/run_qemu.sh
```

`run_qemu.sh` is driven entirely by environment variables — `NEODCT_SNAPSHOT`,
`NEODCT_VERITY`, `NEODCT_SD`, `NEODCT_MODEM`, `NEODCT_NET`, `NEODCT_DEBUG`.
Read its header before adding a flag; the one you want probably exists.

**Give the VM 72 MB.** That comes out to about 55 MB usable, which is what the
32-bit Luckfox actually has after the kernel and CMA take their share. Testing
at 512 MB tells you nothing about whether it fits.

### Real hardware — Luckfox Pico Mini B

```sh
cd buildroot
make luckfox_pico_mini_defconfig
make O=../build-luckfox
```

Images land in `output/images/`. `neodct/tools/mknand.sh` builds the NAND set;
`docs/PARTITIONS.md` has the layout.

### Rebuilding just NeoDCT

The package builds straight from the working tree, so after editing C:

```sh
cd buildroot && make neodct-rebuild && make
```

No version bump, no hash, no tarball. `NEODCT_SITE_METHOD = local` points at
`../neodct/src` and rsyncs it in on every build.

---

## 3. What changed in 0.4.0, build-wise

**musl instead of glibc.** Both defconfigs now set
`BR2_TOOLCHAIN_BUILDROOT_MUSL=y`. Buildroot's default was glibc, which nothing
had ever overridden.

**The screen driver is compiled, not shipped.** `neodctDisplay.c` used to sit
in the overlay next to a committed ARM binary that somebody had built by hand.
That binary was glibc-linked, so under musl its loader
(`/lib/ld-linux-armhf.so.3`) does not exist and the kernel cannot start it at
all. The source now lives in `neodct/src/displayd/` and is built on every
`make`, installing to the same path — so `S90display` and `mkinitramfs.py`
needed no changes.

It builds without `-Wconversion`: the 24 warnings there are ordinary driver
byte-arithmetic in code proven on real hardware, and editing a working SPI
driver to silence them would risk more than it buys. Every other warning,
including `-Werror`, applies.

**Python is still in the image.** It has to be until every app is C —
`SESSION-SCOPE.md` lists what is still stubbed. One trap when that day comes:
`BR2_PACKAGE_SQLITE` is currently reachable *only* through
`BR2_PACKAGE_PYTHON3_SQLITE`. Drop Python without adding sqlite explicitly and
you get a clean build and a phone with no phone book, no messages and no call
log. `package/neodct/Config.in` already selects it, which closes that hole.

---

## 4. Checking the memory

That is the whole point of 0.4.0, so measure it rather than assuming.

On the device, or in QEMU:

```sh
pfetch                    # what you already use
free -m                   # total system
cat /proc/$(pidof nd-core)/smaps_rollup    # just the OS process
```

`smaps_rollup` is the honest one for the UI process. `Rss` is what it has
mapped; `Pss` divides shared pages by how many processes share them, which is
the fairer figure once apps are running as separate programs.

Measured on the build host, same method for both:

```
                     Rss        Pss     Private_Dirty
  Python 0.3.13a   20512 kB   18823 kB
  C 0.4.0           4652 kB    3008 kB      480 kB
                    -----------------------------
                    3.7x       6.3x
```

The `Private_Dirty` figure is the interesting one: 480 kB is all that genuinely
cannot be shared with another process. Everything else is `libneodct.so`,
FreeType and the C library, mapped once and shared by the core and every
running app. That is what makes process-per-app affordable — a second Python
interpreter would have cost 20 MB.

Host numbers are not device numbers. 32-bit ARM pointers are half the size and
Thumb-2 code is denser, so expect the device to come out lower still.

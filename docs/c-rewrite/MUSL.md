# Switching to musl

The project owner asked for musl as a middle ground between glibc and uClibc-ng.
It is the right call, and it is worth roughly 1–2 MB on its own. This records what
has to happen first, because one thing in the tree blocks it outright.

---

## Where we are today

Neither defconfig selects a C library, and **Buildroot's default is glibc**. So the
phone currently runs glibc, not uClibc as was assumed earlier in the project.

Switching means adding one line to both copies of both defconfigs
(`neodct/configs/` and `buildroot/configs/` — see the "two copies" gotcha in
`AGENTS.md`):

```
BR2_TOOLCHAIN_BUILDROOT_MUSL=y
```

`BR2_PACKAGE_MUSL_ARCH_SUPPORTS` covers ARM, so the target is fine.

---

## The blocker: `neodct_displayd` is a prebuilt glibc binary

`neodct/overlay/NeoDCT/System/hw/neodct_displayd` is **committed to the repository
as a compiled ARM executable**, not built from source during the build:

```
ELF 32-bit LSB pie executable, ARM, EABI5, dynamically linked,
interpreter /lib/ld-linux-armhf.so.3, for GNU/Linux 6.12.0
NEEDED  libc.so.6
NEEDED  ld-linux-armhf.so.3
```

On a musl system `/lib/ld-linux-armhf.so.3` does not exist. The kernel cannot even
start the process — it fails at exec, before `main()`. And because
`neodct/scripts/mkinitramfs.py:306` copies this same binary into the initramfs, a
musl switch breaks **both** the running system and the boot-time update applier.

There is no runtime workaround worth having. Shipping a glibc loader alongside musl
defeats the entire purpose of the switch.

**The fix is straightforward, and the source is already here.** `neodctDisplay.c`
(654 lines) sits next to the binary. It needs to be compiled by the build rather
than committed, which is `WP-28 — Display daemon into the package` in the port
plan. That work package is therefore not an independent tidy-up: **it is a hard
prerequisite for musl**, and the plan should be read that way.

Once it builds from source it picks up whatever libc the toolchain is using, and
this problem disappears permanently — which is a good argument for doing it even
if musl were not on the table. A committed binary that nothing rebuilds is a
liability regardless.

Nothing else in the overlay is affected: `neodct-sdcard`, `atcmd` and `pfetch`
are all shell scripts.

---

## Other things to check before flipping the switch

**Python and Pillow must keep working during the transition.** The image carries
both Python and C until the last app is ported, so musl has to support the current
stack as well as the new one. Buildroot builds `python3` and `python-pillow`
against musl routinely, so this is expected to be fine — but it is expected, not
verified, and a full build is the only way to know.

**The Rockchip vendor kernel and any SDK blobs.** The hardware target builds
through the Rockchip SDK (`AGENTS.md`: Ubuntu 22.04 distrobox). Kernel modules do
not care about libc, but any prebuilt vendor userspace binary would hit exactly the
same wall as `neodct_displayd`. Audit the SDK output for dynamically linked
executables before the first musl image.

**Known musl-versus-glibc behaviour differences that matter to this codebase:**

| Area | Difference | Where it bites |
| --- | --- | --- |
| `sin`/`cos` last-bit results | musl and glibc disagree by an ULP | CubeBench wireframe — already covered by the frame tolerance policy in `OPEN-QUESTIONS.md` |
| `malloc_trim(0)` | glibc-only, absent in musl | Koki's `teardown()` calls it; harmless no-op today, must not become a link error |
| Default thread stack | musl's is much smaller (128 KB) | any deep recursion or large stack frame in a thread; the coding standards already ban VLAs and unbounded recursion, which is most of the protection |
| DNS resolution | musl's resolver is simpler; no `/etc/nsswitch.conf` | the update downloader and NTP; worth an explicit test |
| Locale | musl is essentially C/POSIX only | the UI is English-only, so no impact expected |

The thread stack size is the one most likely to produce a confusing crash. Set it
explicitly with `pthread_attr_setstacksize()` on the modem and clock threads rather
than relying on the default, and the difference stops mattering.

---

## Verifying without a full image build

A Buildroot rebuild takes hours, so the fast check is `musl-gcc` on the host — now
installed and working here. Every C module should build clean under it:

```sh
make CC=musl-gcc
```

That catches glibc-only functions and headers at the point they are written, rather
than at the first cross-build. It does not catch ARM-specific issues (alignment,
`-Wconversion` on 32-bit `long`), which is what the cross-build and the on-device
run in Rung 7 are for.

A statically linked musl hello-world on this host is **24 KB**, which is the floor
for what the C binaries can weigh.

---

## Recommended order

1. Build `neodctDisplay.c` from source in the Buildroot package (WP-28). Delete the
   committed binary. Verify the initramfs still gets a working daemon.
2. Keep every C module building clean under `musl-gcc` on the host, continuously,
   from the first commit — it is nearly free if done from the start and miserable
   to retrofit.
3. Set thread stack sizes explicitly.
4. Flip `BR2_TOOLCHAIN_BUILDROOT_MUSL=y` in all four defconfig copies and do a cold
   build. Expect the first one to surface two or three surprises.
5. Measure. The saving should show up as a smaller image and lower idle RSS; if it
   does not, something is still pulling glibc in.

Step 4 is deliberately last. Doing it early makes every unrelated build failure
look like a musl problem.

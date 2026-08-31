# Install progress on the panel — implementation plan

*Scope: `neodct/initramfs/init`, `neodct/initramfs/ndsys-apply.sh`, a new
`neodct/initramfs/ndsys-panel.sh`, new `neodct/src/tools/nd_bootbar.c` +
`nd_bootfb.[ch]` + `nd_bootfont.c`, `neodct/src/Makefile`,
`neodct/scripts/mkinitramfs.py`, and four test files.*

*Written as a plan. It has since been implemented; numbers that were measured
are marked as measured, and numbers that were estimates say so. Section 8
records what the implementation did differently and why.*

---

## The problem

`neodct/initramfs/init` puts `bootlogo.raw` on the panel and then calls
`apply_pending`. That function verifies a signature, hashes ~51 MB, writes ~51 MB
to flash, and hashes ~51 MB back off the flash. For the whole of that the screen
shows one unchanging picture, which is pixel-for-pixel what a hung phone looks
like. The Update app has already told the owner "It takes about a minute and the
screen stays dark for part of it" (`apps/Update/main.c`,
`nd_update_restart_page`) — an apology for exactly this, and one this work makes
unnecessary.

The failure paths are worse than the slow path. Every refusal inside
`apply_pending` logs to `/dev/console` — a serial cable the owner does not have —
and then boots the old system. A package that is not signed by the release key is
refused silently, from the owner's point of view: they install an update, the
phone restarts, and nothing has changed. That is the single most important thing
this feature has to fix, and it is not the progress bar.

---

## 1. Where the progress comes from

### 1.1 The phases are not one operation

`apply_pending` does five things of wildly different cost. Read off the source
(`ndsys-apply.sh` lines 597–683), for a package installed from an SD card, which
is the phone's normal path:

| # | Step | Code | Bytes moved | Bounded by |
| --- | --- | --- | --- | --- |
| a | signature | `release_signature_ok` → `nd-verify` | ~1 KB | one RSA verify |
| b | size | `package_image_size` → `unzip -l` | central directory | nothing |
| c | pre-write hash | `package_image_sha` → `unzip -p … \| sha256sum` | 51 MB | card read + SHA-256 |
| d | write | `unzip -p … \| write_system` | 51 MB | **SPI NAND write** |
| e | read-back hash | `hash_prefix` → `dd \| sha256sum` | 51 MB | NAND read + SHA-256 |

`rootfs.squashfs` is stored with `ZIP_STORED` (`mkupdate.py:120`, "a squashfs is
already compressed"), so `unzip -p` is a copy, not an inflate. There is no hidden
decompression cost in (c) or (d).

(a) and (b) are milliseconds. (c), (d) and (e) each stream exactly the same
number of bytes, at three very different rates. I have no hardware here and
cannot measure them; the honest ordering, from what the parts are, is that (d) is
much the slowest — a static UBI volume update on SPI NAND, with erases — and (c)
is the fastest, since the SD read and a software SHA-256 on a Cortex-A7 are both
faster than NAND programming.

### 1.2 One bar, three bars, or a weighted bar

**A single bar over `3 × image_bytes` is wrong.** The three phases move equal
byte counts at unequal speeds, so such a bar sprints to 33%, appears frozen for
most of a minute, then finishes quickly. People read a bar's *rate* as an
estimate of the time left. A bar that stalls at one third is a worse lie than no
bar, because it is a specific lie.

**A single bar weighted by measured phase cost is not available honestly.** It
needs per-phase durations. Those could be measured with `date +%s` and cached in
`$STATE_DIR/phase_cost.prop` for the *next* install, but the first install on any
phone — which is the one people are most likely to interrupt — would still have
to use a guess, and I cannot measure the guess. Writing a fabricated weight into
a system whose whole point is not to fabricate is the wrong trade. It is also
extra state on the writable partition, which the security work has spent this
release trying to trust less.

**Therefore: one bar per phase, three of them, each labelled.** Each sweep is
derived from exact byte counts, is monotone, needs no calibration, and is correct
on the first boot on hardware nobody has measured. The bar resetting to zero is
explained by the step label above it changing — which is precisely the idiom the
running system already uses, `nd_progress_set_step()` in
`apps/Update/main.c:1012` and `apps/Downgrade/main.c:403`. The boot screen and
the in-system update screen then behave the same way, because they are the same
screen.

Phases (a) and (b) get no bar. They get the **first frame**, drawn before the
signature check runs, with the step label up and the bar at 0%. That is what
makes something appear on the panel within a fraction of a second of the update
starting, rather than after the first megabyte.

| Frame | Step label | Bar | Drawn where |
| --- | --- | --- | --- |
| open | `Checking the update` | 0% | after the attempts bump, before `release_signature_ok` |
| 1 of 3 | `Checking the update` | fills | during `package_image_sha` |
| 2 of 3 | `Installing` | fills | during `write_system` |
| — | `Installing` | 100% | before the `sync` that follows the write |
| 3 of 3 | `Checking the phone` | fills | during the read-back `hash_prefix` |
| done | `Installing` | 100% | left on the panel; boot continues |

The wording is a proposal, not a decision. It should match the Update app's
existing vocabulary ("Checking for updates", "Backing up data", "Preparing
update") and is worth a second opinion before it ships.

### 1.3 How a percentage is actually derived

**Numerator: a counting stage in the pipeline.** A small program that copies
stdin to stdout and counts the bytes as they pass, drawing the bar itself. Every
one of the three long phases is already a pipeline, so the stage drops in:

```sh
# phase 1 — was: unzip -p "$image" rootfs.squashfs | sha256sum
unzip -p "$image" rootfs.squashfs 2>/dev/null \
    | progress_filter "Checking the update" 1 "$image_bytes" \
    | sha256sum | cut -d' ' -f1

# phase 2 — was: unzip -p "$image" rootfs.squashfs | write_system "$image_bytes"
unzip -p "$image" rootfs.squashfs 2>/dev/null \
    | progress_filter "Installing" 2 "$image_bytes" \
    | write_system "$image_bytes"

# phase 3 — inside hash_prefix
dd if="$1" bs=4096 count="$blocks" 2>/dev/null \
    | progress_filter "Checking the phone" 3 "$((blocks * 4096))" \
    | sha256sum | cut -d' ' -f1
```

The count is exact rather than sampled: it is the bytes that actually crossed the
pipe, at the exact point they crossed it.

**Denominator: `image_bytes` from `pending.prop`.** This is not a guess either.
By the time the bar is drawn, `record_matches_manifest()` has proved the record
agrees with a manifest signed by the release key, and `actual_size !=
image_bytes` has already discarded the update if the zip member is a different
size. The number the bar divides by is the number the release was signed for.

For phase 3 the denominator is `blocks * 4096`, **not** `image_bytes`.
`hash_prefix` computes `blocks=$((bytes / 4096))` and `dd` therefore emits
`blocks * 4096` bytes. Passing `image_bytes` would leave the last bar topping out
at 99% on any image that is not 4096-aligned.

*(Observation, not a change: that same truncation means the read-back verifies
slightly fewer bytes than were written when the image is not a multiple of 4096.
Squashfs images are 4 KiB aligned in practice, so it has never bitten. It is
pre-existing and out of scope here.)*

**Percent: the same expression as the widget.**
`percent = trunc(done * 100 / total)`, clamped to `[0, 100]` — literally the line
in `nd_progress_draw()` (`lib/nd_progress.c:156`), so the boot bar and the
in-system bar round identically. `total == 0` means 100%, as it does there.

### 1.4 Mechanisms considered and rejected

- **`dd status=progress`.** GNU dd has it; busybox dd does not. Not available.
- **`kill -USR1` on busybox dd.** `CONFIG_FEATURE_DD_SIGNAL_HANDLING=y` in
  `buildroot/package/busybox/busybox.config`, so busybox dd would print a status
  line to stderr on SIGUSR1. Rejected on two counts: it covers only the QEMU
  path, since the phone's system partition is written by `ubiupdatevol` and not
  by dd at all (`ndsys-apply.sh:520`), and parsing a human-readable line out of
  stderr from a background poller in ash is fragile in a place that must not be.
- **Splitting the stream into 1 MiB chunks in the shell.** Cannot work for the
  phone: `ubiupdatevol -s N vol -` opens one transaction over one stream, so
  there is no seek-and-write to chunk. It also forks 51 times per phase and has
  to cope with short reads from a pipe.
- **`/proc/<pid>/io`.** `rchar`/`wchar` would give a uniform sampled progress for
  dd, `ubiupdatevol` and `sha256sum` alike, with no new binary. Rejected because
  it needs `CONFIG_TASK_IO_ACCOUNTING` in the kernel, **and a kernel cannot ship
  in a `.ndsw`** (AGENTS.md: "There is no kernel in the package"). If the
  deployed kernel lacks it, every phone in the field shows a dead bar and no
  update can ever fix it. A dependency that cannot be repaired over the air is
  not a dependency worth taking for a cosmetic feature.
- **`/proc/<pid>/fdinfo/0`'s `pos:`.** Works only when stdin is a regular file,
  i.e. only for a loose staged image, not for the card path.

The counting filter is the only mechanism that is exact, needs no kernel
feature, and behaves identically for `dd` and `ubiupdatevol`, on QEMU and on the
phone. That is what justifies a new binary.

---

## 2. What draws it

libneodct does not exist in the initramfs and cannot: it needs FreeType, the
`font.ttf` in `/NeoDCT/System/ui/resources/fonts/`, the wallpaper machinery and
an `nd_ui`. The panel here is a raw framebuffer, exactly as in recovery mode.

### 2.1 The seam — what recovery and this should share

Two layers, split so the recovery-mode workstream and this one can share the
lower one:

**Layer A — `neodct/src/tools/nd_bootfb.c` + `nd_bootfb.h`. The primitive.**
Nothing about updates in it. What I want from it, and what I will write if
recovery does not:

```c
typedef struct { /* opaque-ish; fd, mmap, geometry, shadow buffer */ } nd_bootfb;

bool nd_bootfb_open(nd_bootfb *fb, const char *path);   /* false = draw nothing */
void nd_bootfb_clear(nd_bootfb *fb);
void nd_bootfb_fill(nd_bootfb *fb, nd_rect r, bool white);
void nd_bootfb_outline(nd_bootfb *fb, nd_rect r);
int32_t nd_bootfb_text_w(const char *s, uint8_t size);
void nd_bootfb_text(nd_bootfb *fb, int32_t x, int32_t y, const char *s, uint8_t size);
void nd_bootfb_present(nd_bootfb *fb);                  /* one memcpy of the band */
void nd_bootfb_close(nd_bootfb *fb);
```

Required properties, all of which matter to both callers:

1. **Geometry is read, never assumed.** `FBIOGET_VSCREENINFO` for `xres`,
   `yres`, `bits_per_pixel`. QEMU boots with `video=Virtual-1:240x175M` on
   virtio-gpu (`tools/run_qemu.sh:216`); the phone's fb0 is vfb, forced to
   240x175 by `neodct_displayd`'s `init_framebuffer()`, which accepts 16 or 32
   bpp. Both must work. Draw into a 240x175 shadow and blit it top-left, which
   is what `cat splash.raw > /dev/fb0` effectively does today.
2. **Monochrome only.** White and black are byte-order agnostic, so the
   red/blue-order problem `mkinitramfs.py:93` documents for the splash — "ON THE
   PHONE THIS IS THE WRONG WAY ROUND AND IT DOES NOT MATTER YET" — cannot arise.
   No colour, ever, in this layer.
3. **It can never fail its caller.** `nd_bootfb_open()` returning false makes
   every later call a no-op. There is no error path a caller has to handle,
   because the caller is in the middle of writing a system partition and must
   not care about a screen.
4. **Full-frame repaint on present.** 240×175×4 = 168 KB. This is deliberate:
   the framebuffer console is bound to the same fb0 (recovery relies on that,
   `ndsys-recovery.sh:8`), so a kernel message printed during a UBI erase can
   scribble across the bar. A frame that repaints everything heals on the next
   percentage step. `neodct_displayd` diffs against the previous frame and skips
   identical ones (`neodctDisplay.c:466`, `render_dirty`), so repainting
   unchanged pixels costs no SPI traffic at all.

**Layer B — `neodct/src/tools/nd_bootbar.c`.** The update-specific part: argv,
the counting filter, the phase labels, and the `nd_progress` layout.

If the recovery workstream produces a primitive with those five properties under
another name, I take theirs and delete `nd_bootfb.[ch]`; the seam is one header.
If it produces nothing, this stands alone. What must not happen is two
framebuffer openers in one initramfs that disagree about bit depth.

### 2.2 The layout is the widget's layout

`lib/nd_progress.c` computes five boxes from `nd_ui_width()` and
`nd_ui_content_bottom()`. On this panel they are, and the file says so in a
comment because `test_update_ui.py` asserts on them:

```
header_box (0, 4, 240, 19)      divider_y 24
label_box  (0, 44, 240, 65)
bar_box    (20, 79, 220, 93)    ND_PROGRESS_INSET 2
status_box (20, 102, 220, 117)
hint_box   (0, 124, 240, 139)
```

`nd_bootbar` hardcodes those, because there is no `nd_ui` to derive them from.
Hardcoding is only acceptable with a check, so: **`test/unit/test_bootbar.c`
constructs a real 240x175 `nd_ui`, calls `nd_progress_init()`, and asserts the
boxes equal `nd_bootbar`'s constants.** If someone changes the panel size or
`bar_top = trunc(content_bottom * 0.55)`, that test fails and names the boot
screen. There is precedent for exactly this kind of cross-check:
`test_the_verity_table_matches_what_the_python_side_computes` in
`test_initramfs_apply.py` pins `verity_table()` against `verity.py`.

Contents, matching the widget:

- header, 14 px: `SOFTWARE UPDATE` at x=10 (the Update app's
  `ND_UPDATE_HEADER`), with `2/3` right-aligned on the same line, and the
  one-pixel divider at y=24.
- label, 20 px, centred: the step name.
- bar: 1 px outline of `bar_box`, fill inset by 2, `filled = trunc(span *
  percent / 100)` — the widget's arithmetic exactly.
- status, 14 px: `45%` left, `24.1 of 51.0 MB` right — the same split
  `nd_progress_draw` uses when a detail function is supplied.
- hint, 14 px, centred: `Do not power off`.
- rows 146–174 stay black. `nd_progress_draw` ends with
  `nd_softkey_update(&bar, "", false)`, which clears the strip; there is nothing
  to press during a boot install, so the boot screen simply never draws there.

### 2.3 The font

`nd_font.c` is FreeType over `font.ttf`. Neither is in the initramfs, and putting
them there would cost roughly a megabyte of RAM on a 64 MB device, unpacked from
a cpio that on the Luckfox is built into the kernel image.

**Preferred: generate 1-bit glyph tables from the real typeface at build time and
commit them.** A host generator, `neodct/src/tools/gen_bootfont.c`, links
libneodct's `nd_font`, renders printable ASCII (0x20–0x7E) at 14 px and 20 px,
thresholds the 8-bit coverage at 128, and writes `neodct/src/tools/nd_bootfont.c`
— glyph bitmaps plus the advance and ink offset for each, so the boot screen's
text metrics are the phone's text metrics and the centring lines up with
`nd_text_size`. Cost: about 8 KB of `.rodata`, a couple of KB gzipped. The
generated file is committed, and a test re-runs the generator and compares, the
same way `fontref.json` pins the font renderer.

**Fallback if that proves fiddly: a fixed 8×8 ASCII table, 768 bytes, written by
hand.** It works, it is legible at 30 columns, and it looks like a PC BIOS on a
phone that has spent a lot of effort not looking like one. Take it only if the
generator turns into a project of its own.

Either way the boot screen's glyphs are 1-bit and the OS's are antialiased, so
the two screens will not be pixel-identical. The layout will be.

---

## 3. The plumbing

### 3.1 `neodct/initramfs/ndsys-panel.sh` — new file

Move `panel_start`, `panel_show`, `panel_stop` and the `PANEL_*` defaults out of
`ndsys-recovery.sh` verbatim, and add the progress helpers. Reasons: `init`
already calls `panel_start` from `panic()` behind a `command -v` guard because it
may not have been sourced; `apply_pending` needs the same helpers; and recovery
needing the panel is not a reason for the applier to depend on the recovery
menu. `init` sources it before both. `mkinitramfs.py` copies every *file* in
`neodct/initramfs/` verbatim (`mkinitramfs.py:463-467`), so a new `.sh` needs no
build change, and `test_initramfs_applets.py` scans it automatically because
`script_paths()` lists the directory.

New content:

```sh
: "${NDSYS_BOOTBAR:=/bin/nd-bootbar}"

# A pipeline stage. exec, so this replaces the subshell the pipeline already
# made rather than adding a process.
progress_filter() {   # progress_filter STEP PHASE TOTAL
    if [ -n "$PANEL_UP" ] && [ -x "$NDSYS_BOOTBAR" ]; then
        exec "$NDSYS_BOOTBAR" --step "$1" --phase "$2" --total "$3"
    fi
    exec cat
}

progress_frame() { … }   # one-shot: --at 0 / --at 100
progress_fail()  { … }   # the failure screen, held PANEL_SPLASH_HOLD seconds
```

**`exec cat` is the whole safety story.** When the tool is missing, when the
panel never came up, and — this is the important one — **in every host test**,
where there is no `/dev/fb0` and no built binary, the pipeline degrades to a
`cat` that changes neither its shape nor its exit status. ash reports a
pipeline's status from its last command, and none of these scripts sets
`pipefail` or `set -e` (checked). Every existing test in
`test_initramfs_apply.py` therefore keeps passing without being touched, which is
the bar a cosmetic change to this file has to clear.

### 3.2 `neodct/initramfs/ndsys-apply.sh` — the call sites

- `apply_pending`, after the attempts bump and before `release_signature_ok`:
  `progress_frame "Checking the update" 1 0`. This is the first pixel change, and
  it is also where `panel_start` is called if the boot logo never did it (it is
  idempotent — `[ -z "$PANEL_UP" ] || return 0`).
- `package_image_sha` and the loose-image `hash_prefix` on the pre-write hash:
  filter, phase 1.
- The two `write_system` call sites: filter, phase 2. `write_system` itself is
  not touched — the filter goes on its stdin, so the UBI and dd branches both get
  it for free and neither knows.
- Between the write and `sync`: `progress_frame "Installing" 2 100`. A 51 MB
  `sync` with the bar sitting at 99% would look like the hang this is meant to
  remove.
- `hash_prefix` on the read-back: filter, phase 3, total `blocks * 4096`.
  `hash_prefix` is also called by `recovery_install_package`, so the phase label
  and total are parameters with a default that draws nothing — recovery's
  behaviour is unchanged until recovery opts in.

The functions are called through `$NDSYS_BOOTBAR` and guarded by `command -v`
where `ndsys-panel.sh` might not be sourced, matching the existing idiom in
`init`'s `panic()`.

### 3.3 Failure

Today a refusal writes `last_result.prop`, logs to a serial console, and boots
the old system. The panel says nothing. Each refusal gets a screen: the same
frame, the step label replaced by a headline, the bar drawn as an empty outline,
the reason in the status line, held for `PANEL_SPLASH_HOLD`.

| `apply_pending` path | Headline | Status line | Hold |
| --- | --- | --- | --- |
| incomplete staging record | `Update not installed` | `The update was incomplete` | 3 s |
| **not signed by the release key** | `Update refused` | `Not signed by NeoDCT` | 5 s |
| staged image truncated | `Update not installed` | `The update is damaged` | 3 s |
| sha256 mismatch before write | `Update not installed` | `The update is damaged` | 3 s |
| write failed | `Install did not finish` | `It will try again` | 3 s |
| read-back mismatch | `Install did not finish` | `It will try again` | 3 s |

The refusal gets the longest hold because it is the one with a person behind it:
somebody handed this phone a package that is not a NeoDCT release, and the owner
has to be told to their face rather than in `last_result.prop`. Text is kept
short enough to fit; the long reason still goes to `record_result`, and the
Update app still shows it on the next launch through
`nd_update_report_last_result()`. Seeing it twice is correct.

Two of these lie about their own consequences if we are not careful. A
**read-back mismatch** means the system partition has already been overwritten
with something that does not hash right — `installed.prop` still describes the
*old* image, so `setup_verity` will fail and `init` will call
`recovery_or_panic`, which puts the sad face up and enters recovery. "It will try
again" is true (the pending record is kept) but the next thing the owner sees is
recovery. That is a pre-existing property of the applier, and recovery's own
screen takes over; noted so nobody wonders later.

**On success nothing is held.** The 100% frame stays on the panel by itself: as
`init` says at line 304, "The logo stays on the panel — nothing clears it — until
the UI draws over it". Verity, the squashfs mount, `switch_root` and the whole UI
coming up take seconds, and the finished bar sits there for free. A successful
update boot gets no slower.

### 3.4 `neodct/src/Makefile`

Mirror the `nd-verify` block exactly (lines 300–324, 485–488):

```make
BOOTFB_SRCS  := $(wildcard tools/nd_bootfb.c tools/nd_bootfont.c)
BOOTBAR_SRCS := $(wildcard tools/nd_bootbar.c)
TARGETS      += $(if $(BOOTBAR_SRCS),$(BINDIR)/nd-bootbar)
$(BINDIR)/nd-bootbar: $(BOOTBAR_OBJS) $(BOOTFB_OBJS)      # static, dynamic fallback
install-boot: … $(INSTALL) -m 0755 $(BINDIR)/nd-bootbar $(BOOTDESTDIR)/ ; $(STRIP) …
```

Static preferred for the same reason nd-verify is, with the same "static failed,
linking dynamically" note. Unlike nd-verify this links nothing but libc — no
OpenSSL, no libneodct — so statically against musl it is tens of kilobytes, not
4.3 MB. Add `tools/nd_bootfont.c` to the `format` target's file list, or exclude
it there: it is generated, and clang-format will fight the generator.

### 3.5 `neodct/scripts/mkinitramfs.py`

Add `nd-bootbar` next to the panel daemon, **optional in the same way**:

```python
BOOTBAR_CANDIDATES = ("NeoDCT/System/bin/nd-bootbar", "usr/bin/nd-bootbar",
                      "bin/nd-bootbar")
```

Found → `binaries["bin/nd-bootbar"]`, with the same `elf_machine()` check the
panel daemon gets. Missing → print to stderr and continue. It must **not**
`sys.exit()` the way the missing verifier and missing dmsetup do: those make the
initramfs unable to do its job safely; a missing progress bar makes it silent,
which is where we are today. Failing an image build over a cosmetic binary is the
wrong default. The risk that it then silently never ships is covered by a test in
§4.

---

## 4. Tests

1. **`neodct/tests/test_initramfs_progress.py` (new).** Drives the real
   `apply_pending` with `NDSYS_BOOTBAR` pointed at a shell stub that appends its
   argv to a log and `exec cat`s. Asserts: the three phases appear in order with
   the right step labels; phases 1 and 2 are given `image_bytes` and phase 3 is
   given `blocks * 4096`; the byte count the stub passes through equals
   `image_bytes`; the failure frames are drawn on each refusal path. Uses the
   existing `stage_an_update` / `gate_env` fixtures from
   `test_initramfs_apply.py`.
2. **The regression that actually matters, same file:** with `NDSYS_BOOTBAR`
   unset, non-existent, and non-executable, the install still succeeds and
   `installed.prop` is byte-identical to what it is today.
3. **`neodct/src/test/unit/test_bootbar.c` (new).**
   - the five boxes match `nd_progress_init()` on a real 240x175 `nd_ui` (§2.2);
   - `percent` matches `nd_progress_draw()` over a table of `(done, total)`
     including `total == 0`;
   - **with `--fb` pointing at a path that cannot be opened, stdin is still
     copied to stdout byte-for-byte and the exit status is 0.** This is the
     property that stops a cosmetic feature from failing an install, and it is
     the reason the tool is a filter rather than a poller;
   - a stream whose length is not a multiple of the read chunk copies exactly;
   - short `write()`s on stdout are retried rather than dropped.
4. **`neodct/tests/test_initramfs_applets.py`.** Add `nd-bootbar` to
   `EXTRA_BINARIES`. It is invoked through `$NDSYS_BOOTBAR`, so the static
   scanner cannot see it — the file already handles exactly this case for
   `ubiupdatevol` by naming it directly, with a comment saying why.
5. **`neodct/tests/test_mkinitramfs.py`.** A target tree containing
   `nd-bootbar` puts it in the cpio; a tree without it still builds, and warns.

Nothing here needs hardware. What needs hardware, and should be said plainly in
the commit: nobody can confirm the SPI NAND write rate, the real duration of each
phase, or that the bar looks right on the ST7789 until this runs on a Luckfox.
QEMU can confirm the pipeline, the arithmetic and the pixels on virtio-gpu.

---

## 5. Cost

Everything below is arithmetic on measured constants, not benchmarks.

- **Processes:** three more per install, one per phase, each `exec`'d into a
  pipeline subshell that already existed. Zero when the tool is absent (`exec
  cat` replaces the same subshell).
- **Pipe copies:** 3 × 51 MB of extra memcpy and ~2400 extra 64 KiB pipe
  transactions. Against a phase whose bottleneck is NAND programming this is well
  under a percent; the estimate is a few hundred milliseconds total on a 1.2 GHz
  Cortex-A7.
- **Frames drawn:** the widget's percent gate, ported literally — draw only when
  the whole percentage changes. At most 100 frames per phase, 300 per install.
  That is comfortably inside the brief's "per megabyte is fine, per block is
  not": 51 MB over 100 frames is one frame per 512 KB.
- **Framebuffer writes:** 168 KB per frame at 32 bpp, 300 frames, ≈50 MB of
  memcpy across the whole install — order of 100 ms, and it buys immunity to
  fbcon scribbling on the bar.
- **SPI traffic on the phone:** none added. `neodct_displayd` already polls at 30
  fps and sends only the changed bounding rectangle; the changed region here is
  the bar's leading edge plus the status line — the size of a clock tick, which
  the daemon's header calls "a few KB, not 115 KB". Repainted-identical pixels
  are skipped by `memcmp` before any conversion happens.
- **cpio:** `nd-bootbar` static against musl plus ~8 KB of font tables — tens of
  kilobytes, against the 4.3 MB `nd-verify` already packed in. On the Luckfox
  this lands in the kernel image, where the boot partition is 16 MB.
- **Boot time:** unchanged on a boot with no pending update (nothing is drawn and
  nothing is started). Unchanged on a successful install — the final frame is
  free. Longer by 3–5 s on a failed install, deliberately, so the failure can be
  read.

---

## 6. Knock-on change

`apps/Update/main.c`, `nd_update_restart_page()`: "It takes about a minute and
the screen stays dark for part of it." That stops being true. It should say the
screen shows how far along it is. One string, one test in `test_update_ui.py` if
it asserts on the body, and a `CHANGELOG.txt` line under `Unreleased` — an owner
watching an update install is exactly the kind of thing that belongs there.

---

## 7. Order of work

1. `ndsys-panel.sh` — move the panel helpers, add no-op progress helpers. Nothing
   changes on screen; `test_initramfs_recovery.py` must still pass.
2. `nd_bootfb.[ch]` + the font, and `test_bootbar.c`'s geometry cross-check.
3. `nd_bootbar.c` as a filter, plus the "unopenable fb still copies" test. This
   is the step where the risk lives, so it is tested before it is wired in.
4. Makefile, `install-boot`, `mkinitramfs.py`, their tests.
5. Wire the three phases into `ndsys-apply.sh`; `test_initramfs_progress.py`.
6. The failure screens.
7. QEMU: build an update, `run_qemu.sh`, watch it install.
8. The Update app string and the changelog.

Steps 1–4 change nothing an owner can see, and step 5 is the first that does. If
this has to stop early, it should stop at a step boundary, and every one of them
leaves the applier installing updates exactly as it does today.

---

## 8. What shipped, and where it differs

Implemented in full. Files: `neodct/initramfs/ndsys-panel.sh` (new),
`neodct/initramfs/ndsys-apply.sh`, `neodct/initramfs/init`,
`neodct/initramfs/ndsys-recovery.sh`, `neodct/src/tools/nd_bootfb.[ch]`,
`nd_bootbar.[ch]` + `nd_bootbar_ui.c`, `nd_bootfont.[ch]`, `gen_bootfont.c`,
`neodct/src/Makefile`, `neodct/scripts/mkinitramfs.py`,
`neodct/scripts/post-image-neodct.sh`, `neodct/tools/bootbar_frames.py` (new),
and five test files.

Six departures, all deliberate:

1. **The font is generated at three sizes, not two.** §2.3 says 14 px and
   20 px. `nd_progress_draw()` does not pick a size, it picks a *rung*:
   `nd_font_ladder()` offers 20, 18, 14 and `nd_fit_font()` takes the first
   that fits. "Checking the update" does not fit at 20 px on a 224 px box, so
   with only two sizes the boot screen would either overflow or drop to 14
   where the Update app drops to 18. 18 px costs 1.5 KB more of `.rodata`.
   `nd_bootfb_ellipsize()` is ported from `nd_text_ellipsize()` for the same
   reason. `test_bootbar.c` checks the rung against `nd_fit_font()`.

2. **The status detail says the unit once.** §2.2's own example is
   `24.1 of 51.0 MB`, which is not what `nd_update_size_detail()` produces
   (`24.1 MB of 51.0 MB`). The spec's shorter form is what shipped, and it had
   to be: measured at 14 px, the long form is 156 px against `100%`'s 41 in a
   200 px box, leaving three pixels between them, which reads as one run-on
   word. The short form measures 126 and the gap is 33.

3. **`nd_bootbar.c` is split in two.** `nd_bootbar_ui.c` holds the layout, the
   arithmetic and the copy loop; `nd_bootbar.c` holds only `main()`. Otherwise
   `test_bootbar.c` cannot link the thing it is testing.

4. **`nd_bootfb_open_at()` exists as well as `nd_bootfb_open()`.** A regular
   file has no `FBIOGET_VSCREENINFO` to answer, and §2.1 property 1 says
   geometry is read and never assumed -- so rather than let the opener invent
   a default when the ioctl fails, the geometry is *passed in* by the two
   callers that have no driver to ask: the host tests and
   `neodct/tools/bootbar_frames.py`. Nothing on a device uses it.

5. **A seventh failure screen.** §3.3's table has six. `apply_pending` has a
   seventh dead end -- `gave up after N attempts` -- and the three boots that
   reach it have each just told the owner "It will try again". Leaving that
   one silent makes the last screen they saw a lie, so it says
   `Update not installed / It has been given up on`.

6. **`neodct/tools/bootbar_frames.py` is new and not in the plan.**
   `nd-shoot` cannot see this screen: it is drawn by a binary with no
   libneodct in it. This is that screen's equivalent, and it is how
   `docs/img/bootbar-*.png` were produced.

### What is still not known

Nobody has run this on hardware. QEMU and the host tests confirm the pipeline,
the arithmetic and the pixels; the SPI NAND write rate, the real duration of
each phase and how the bar looks on an ST7789 are still unmeasured, exactly as
§4 said they would be.

### What the recovery-UI work could take

`nd_bootfb.[ch]` is Layer A as described in §2.1, with all five properties, and
it has nothing about updates in it. If `nd-recui` wants it, take it: the seam
is one header and the boot bar keeps working. What would be wanted back, if
recovery writes its own instead, is a primitive that reads geometry rather than
assuming it, is one bit deep, cannot fail its caller, repaints the whole band,
and measures text the way `nd_text_size()` does -- that last one is what stops
the two screens' layouts from drifting. What must not happen is two
framebuffer openers in one initramfs that disagree about bit depth.

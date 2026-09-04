<!-- Produced by a design pass before implementation; see 0.5.0b-dev history.
     It opens by correcting four things the brief got wrong, which is why it is
     kept verbatim rather than summarised. -->

# On-screen recovery UI — implementation plan

## 0. Four corrections to the premise, first

**(a) Recovery is not serial-only today — its *input* is.** `ndsys-recovery.sh` already draws on `/dev/tty1`, the framebuffer console, and `mkinitramfs.py` already ships `neodct_displayd` so those pixels reach the ST7789. On a phone today you *see* a 30×10 monospace menu. What you cannot do is drive it: the sixteen keys are on a PCF8575 that no kernel driver binds, so no byte ever arrives on the VT and `recovery_tty()`'s `/dev/tty1` branch is a dead end. Recovery on hardware is a menu you can read and cannot operate. **The i2c keypad is the load-bearing half of this task; the graphics are the part that makes it look like the phone.**

**(b) `neodct/src/lib/nd_widgets.c` does not exist.** The list is `lib/nd_vlist.c`, the progress screen is `lib/nd_progress.c`; `include/nd_widgets.h` is the shared header. I read all three.

**(c) "a progress bar for the long operations (erasing the system)" points at the wrong operation.** `recovery_action_wipe_system` is `dd bs=1M count=1` — deliberately instant, with a comment saying so. The genuinely long operation is `recovery_install_package`: three passes over a ~48 MB image (hash-from-zip, write, read-back). That is where the bar belongs, and the design below puts it there. Wipe-system gets a message screen, not a bar, because a bar that fills in 80 ms is a lie.

**(d) The sad face is already done.** `recovery_or_panic()` calls `panel_start && panel_show "$PANEL_SPLASH"` before anything else, on both entry paths (`neodct.recovery=1` and the `boot_recovery` flag). `PANEL_SPLASH` is `/splash.raw`, built by `mkinitramfs.py` from `splash/sadface.bmp`. No new work.

**What is actually available in the initramfs** (I unpacked the built `initramfs.cpio.gz`): `/splash.raw` and `/bootlogo.raw` — 168,000 bytes each, 240×175 XRGB8888, and I checked both source BMPs: **exactly two colours, `000000` and `ffffff`**. Plus `/neodct-release.pub`, busybox, `dmsetup`, `nd-verify`, `neodct_displayd`, and musl `libc.so`. `font.ttf`, the icons and the wallpapers live in the squashfs and are by definition not mounted.

---

## 1. Shape: `nd-recui` is a per-screen UI server; the shell keeps the logic

The existing file already separates "the part that can destroy a system" from "the UI", and unit-tests the first half on the host. Preserve that line exactly. `nd-recui` never mounts, never writes a block device, never unzips. It draws one screen, reads keys, prints the answer on **stdout**, and exits — which makes it a drop-in for `recovery_menu()`, which already echoes an index on stdout.

Five verbs, nothing else:

| invocation | draws | stdout | exit |
| --- | --- | --- | --- |
| `nd-recui menu HEADING ITEM…` | vertical list, white-fill selection | 1-based index, or `0` | 0 |
| `nd-recui confirm QUESTION` | yes/no list, **no** preselected | — | 0 = yes, 1 = no |
| `nd-recui message LINE…` | text page, waits for any key | — | 0 |
| `nd-recui progress --total N --step S [--header H]` | progress bar; **copies stdin→stdout**, counting bytes | the data | 0 |
| `nd-recui splash PATH [HOLD]` | blits a `.raw`, optional dwell | — | 0 |

`progress` as a `pv`-style pipe filter is the piece that makes this cheap. No signals, no polling of `dd`, no busybox `status=progress` dependency:

```sh
unzip -p "$package" rootfs.squashfs \
  | "$RECUI_BIN" progress --total "$image_bytes" --step "Checking image" \
  | sha256sum | cut -d' ' -f1
```

and identically for the `dd` write and for `hash_prefix`'s read-back. All three long passes get a real bar for one added pipeline stage each.

Exit code 2 from any verb means *"I have no usable input device"*. The shell then falls back to the existing tty menu unchanged. That is the whole error strategy.

---

## 2. Files

**New:**

```
neodct/src/recovery/nd_recui.c        main(), the five verbs, argv parsing
neodct/src/recovery/nd_recdraw.c      fb0 open/probe, blit, rect, text, the two widgets
neodct/src/recovery/nd_recinput.c     evdev discovery + PCF8575 matrix scan + keymap read
neodct/src/recovery/nd_recui.h        the internal contract between the three
neodct/src/recovery/nd_recfont.h      GENERATED — do not edit (see §4)
neodct/tools/mkrecfont.py             generates nd_recfont.h from font.ttf
neodct/src/test/unit/test_recui.c     host unit test
neodct/tests/test_recui_font.py       regenerates the header and diffs it (skips without PIL)
```

`recovery/` is a new top-level source directory beside `displayd/`, and that is deliberate: `displayd/` is the existing precedent for *a standalone program in this tree that does not link libneodct and gets its own CFLAGS*. Building it **without `-Iinclude`** (exactly as the displayd rule does) is the enforcement mechanism — a stray `#include "nd_ui.h"` fails to compile rather than quietly pulling in FreeType.

Constants that live in headers we cannot include (`ND_KEY_ENTER` 28, `ND_KEY_CLEAR` 14, `ND_KEY_UP` 103, `ND_KEY_DOWN` 108, `ND_KEY_1`..`ND_KEY_0` 2..11, `ND_KEY_STAR` 42, `ND_KEY_HASH` 43, `I2C_SLAVE` 0x0703, bus 3, addr 0x20, `KDSETMODE`/`KD_GRAPHICS`, `struct input_event`) get copied in with a comment naming the source header — the same move `nd_evdev.c` already makes for `struct input_event` and `EVIOCGNAME`, and for the same reason.

**Modified:** `neodct/src/Makefile`, `neodct/scripts/mkinitramfs.py`, `neodct/scripts/post-image-neodct.sh`, `neodct/initramfs/ndsys-recovery.sh`, `neodct/tests/test_mkinitramfs.py`, `neodct/overlay/NeoDCT/CHANGELOG.txt` (Unreleased).

---

## 3. Output: one code path, both targets, nothing to detect

The single most useful finding: **there is no output difference to detect.**

- QEMU: `video=Virtual-1:240x175M` on the cmdline, so `/dev/fb0` is already 240×175 and *is* the screen.
- Phone: `/dev/fb0` is vfb; `panel_start` in the shell has already launched `neodct_displayd`, which forces 240×175 @ 32bpp (16bpp fallback) and mirrors fb0 to the ST7789 at 30 fps by diffing.

So `nd-recui` opens `/dev/fb0`, `FBIOGET_VSCREENINFO` + `FBIOGET_FSCREENINFO`, honours `line_length` and `bits_per_pixel` (32 and 16 only; anything else → exit 2), mmaps, and draws. That is the same interface `panel_show`'s `cat > /dev/fb0` already uses. `nd_fb.h`'s `line_length == 0 → xres * bpp / 8` fallback is load-bearing on the Rockchip driver and gets copied.

**Colour is not a problem here and must not be made into one.** The two framebuffers genuinely disagree about red's offset (QEMU B G R x, vfb R G B x — `nd_fb.h` documents it at length). Recovery draws only `000000` and `ffffff`, which are identical under any channel permutation, exactly as the two existing splashes are. **The recovery UI is one bit deep, on purpose.** That is a design constraint to write down, not an accident: it deletes the whole class of bug and matches the two assets already in the image.

**`KDSETMODE`/`KD_GRAPHICS` is required, not optional.** With the VT left in its default mode, any key pressed while `nd-recui` is running is *echoed by the kernel onto tty1*, and fbcon paints that text over our framebuffer. So on entry: open `/dev/tty0` (falling back to `/dev/tty1`), `ioctl(KDSETMODE, KD_GRAPHICS)`; on exit, and from a `SIGTERM`/`SIGINT` handler, `KD_TEXT`. Failure is ignored — a build with no VT loses nothing.

---

## 4. The font: a 1-bit bitmap of the phone's own `font.ttf`, two sizes, generated at build authoring time and committed

**Decision: a generated 1-bit ASCII bitmap font extracted from `neodct/overlay/NeoDCT/System/ui/resources/fonts/font.ttf`, at 18 px and 14 px, emitted as a committed C header. 5,126 bytes of `.rodata`.**

I measured rather than guessed. With the layout engine pinned to `BASIC` — which is what the phone uses, because Buildroot's Pillow is built `-Craqm=disable` and the C engine is plain FreeType, and which `fontref.py` already pins for exactly this reason:

| px | glyph bits, 95 chars | max ink | ink dx | advances |
| --- | --- | --- | --- | --- |
| 14 | 1,783 B | 14×15 | **always 0** | all whole pixels |
| 18 | 2,203 B | 18×17 | **always 0** | all whole pixels |
| 24 | 3,645 B | 24×24 | always 0 | all whole pixels |

`ink_dx` is identically zero at every size, so it need not be stored. Record: `{uint8 adv; uint8 w; uint8 h; uint8 dy; uint16 bit_off;}` — 6 B × 95 = 570 B per size. **14 px + 18 px = 1,783 + 2,203 + 1,140 = 5,126 bytes.** Rows packed MSB-first, `(w+7)/8` bytes per row. Coverage thresholded at ≥128 — antialiasing is meaningless in a one-bit design and would cost 8× the bytes.

**Why two sizes, and why not 24.** I measured the real strings:

```
                         24px  18px  14px      (panel is 240 wide)
NeoDCT recovery           258   188   157
wipe user data            219   161   131      widest menu item
1-5 or arrows, Enter      300   221   180      the heading
UPDATE-0.5.0b.ndsw        300   221   181      a real package name
```

The real `VerticalList` puts its title in `font_xl` (24). **"NeoDCT recovery" at 24 px is 258 px wide and does not fit on a 240 px panel** — so the framework's own title size is unavailable here regardless of budget, and shipping a third table would buy nothing. 18 px carries the title (188 px at x=5) and every menu item (161 px at x=10, clear of the 225 px selection-bar edge). 14 px carries the heading, the percentage, the byte reading and package filenames, which overflow at 18 px. Two sizes is the minimum that works and the maximum that is justified.

**Why not pre-rendered text as raw images.** One 240×175 XRGB8888 screen is **168,000 bytes** — 33× the entire font, and 5× the whole binary — and the cpio is unpacked into RAM on a 64 MB device, so uncompressed size is what costs. Worse, it cannot work: recovery must render package filenames read off an SD card, percentages, and byte counts. Text that is not known until runtime is not optional here.

**Why not a hand-authored console font.** It would be ~1.5 KB and need no generator, and it is what recovery looks like today via fbcon's 8×16 — which is precisely the look this task is asking to leave behind. The generator is ~80 lines of Pillow and buys the actual typeface.

**Why the header is committed rather than generated during `make`.** `mkinitramfs.py`'s own BMP reader is hand-rolled specifically because "nothing guarantees PIL is installed" on a build host. That rule applies with equal force here. The repo already has the pattern for the exception: `neodct/tools/fontref.py` and `mkicons.py` are developer-run PIL tools whose **outputs are committed** (`neodct/tests/golden/font/fontref.json`, the icon PNGs). `mkrecfont.py` joins them. `neodct/tests/test_recui_font.py` regenerates and byte-compares when PIL is importable and `pytest.skip`s when it is not — so the header cannot silently drift from `font.ttf`, and no build host gains a dependency.

---

## 5. Input: two backends, probed, never configured

At startup, in this order, and **both** are polled if both open:

1. **evdev.** `readdir("/dev/input")`, open every `event*` `O_RDONLY|O_NONBLOCK|O_CLOEXEC`, cap 8. No `by-path`/`by-id` globbing — there is no udev in the initramfs and those symlinks do not exist, which is why `nd_evdev_discover()`'s six-step order cannot be copied wholesale. Decode `struct input_event` in **both** the 16-byte and 24-byte layouts, as `nd_evdev.c` does, keyed off the read size. `EV_KEY` (1) with value 1 or 2 (accept the kernel's own repeat; see §7). NeoDCT keycodes *are* evdev keycodes, so there is no translation table.

2. **i2c matrix.** Read the keymap (below). If it yields ≥1 key, `open("/dev/i2c-%d", O_RDWR)`, `ioctl(I2C_SLAVE, addr)`. Scan is a direct minimal port of `raw_scan()` + `nd_matrix_scan_once()` from `lib/nd_matrix.c`: for each row pin write `0xFFFF & ~(1u << pin)` low-byte-first, sleep `ND_SCAN_SETTLE_US` = 500 µs, read two bytes, any column bit reading 0 is a closed switch. Keep `ND_RELEASE_SCANS` = 3 for release debounce and press-edge-only reporting. **Drop** the pending-press queue and rollover machinery — a menu needs one key at a time, and `nd_matrix.c`'s own comment says the queue exists for games.

Poll both on a 10 ms tick with `poll()` on the evdev fds and a scan between. First key wins. `nd_pcf8575_close()`'s parting `write16(0xFFFF)` is copied — leaving a row driven low across a restart is a real failure.

**If neither opens, exit 2** and let the shell fall back to its tty menu. Drawing a menu nobody can move is worse than the console text that at least works over serial.

### The keymap

`/mnt/user/keymap.json` (`$MNT_USER` from `init`, i.e. `ND_PATH_KEYMAP` on the mounted user partition). Passed as `--keymap PATH` so the host test can point it elsewhere; default `/mnt/user/keymap.json`.

**Read it once, at startup, before any menu.** `recovery_action_wipe_user` deletes everything on the partition except `.ndsys` — *including `keymap.json`* — so re-reading after a wipe would lose the keypad mid-session. (The first-boot wizard regenerates it on the next boot, so this is not a new bug, but it constrains the ordering here.)

No JSON parser. Four targeted extractions over the ~2 KB file, whitespace-agnostic:

- `"row_pins"` → skip to `[`, `strtol` until `]`
- `"col_pins"` → same
- `"i2c_bus"`, `"i2c_addr"` → skip past `:`, `strtol` (accept `"0x20"` as well as `32`, as `nd_keymap.c` does)
- `"by_matrix"` → skip to `{`, then repeatedly parse `"R,C" : "name"` to the closing `}`

`by_matrix` rather than the nested `keys` object because it is already the flat `row,col → name` map this needs — `nd_keymap_save()` writes both, and both writers (`nd_keymap_save` and `apps/KeypadMapperI2C`) emit `json.dump(indent=2, sort_keys=True)`. Note `"row_pin"` (singular, inside `keys`) cannot collide with `"row_pins"`. This is the same trade-off `recovery_manifest_field()` already takes, with the same justification, and unlike that one it is whitespace-independent.

Name → code table, 16 entries, from `nd_kpsetup_targets[]` and `nd_keycodes.h`: `navikey`→28, `clear`→14, `up`→103, `down`→108, `num_1..num_9`→2..10, `num_0`→11, `star`→42, `hash`→43. Unknown names skipped silently, out-of-range positions dropped — matching `nd_keymap.c`'s deliberately forgiving reader, whose comment ("a keymap missing the '7' key still boots a phone you can fix") applies doubly in recovery.

**No compiled-in default matrix.** `docs/c-rewrite/spec-hw-input.md` shows `row_pins [0,1,2,3]`, `col_pins [4,5,6,7]` and `apps/KeypadMapperI2C` carries those as fallbacks — but pins alone do not say *which switch is Up*, and guessing would move the selection at random on a phone somebody is trying to rescue. With no keymap, i2c input is simply unavailable and the `message` screen says so: `"No keypad map."` / `"Use the serial console."` That is the honest answer.

---

## 6. Drawing: the two shapes, with the framework's real numbers

Derived at runtime from the framebuffer's `xres`/`yres`, then the framework's arithmetic. On this panel (`nd_ui.h`: `ND_UI_W` 240, `ND_UI_H` 175, `content_bottom` 145, header divider 30):

**The list** — ported from `nd_vlist_draw()`:
- clear rows `0..content_bottom` (145) only, black
- title at `x=5, y=0`, 18 px, white
- divider: one white pixel row at `y=30`
- `y_start = 40`, `content_height = 101`, `line_height = max(28, 101/3) = 33`, `item_height = max(24, 33-4) = 29` → rows at 40, 73, 106; `max_lines = 3`
- `bar_x = 235`, `selected_right = max(20, 225) = 225`
- selected row: filled white rect `(0, y, 225, y+29)`, text black at `x=10`; unselected: white text at `x=10`
- text y is `y + max(0, (29 - ink_h)/2)` — **ink** height, per glyph, which is the house look and the reason a font that stores per-glyph `h` is needed
- scrollbar: 1 px **grey** (128,128,128) track from `y=40` to `y=140`, white 5×7 notch at `trunc(top + index*step)`

Grey is the one place the one-bit rule bends. `nd_vlist.c` calls it "the only grey pixels in the entire framework". Options: keep it (needs a third value, and 128,128,128 is channel-order-safe anyway since all three components are equal — so it costs nothing) — **keep it**. Equal components mean any permutation is still grey.

**The progress screen** — ported from `nd_progress_draw()`, same five boxes: `bar_top = trunc(145 * 0.55) = 79`, bar `(20, 79, 220, 93)`, `ND_PROGRESS_INSET` 2, `ND_PROGRESS_BAR_HEIGHT` 14, `ND_PROGRESS_BAR_MARGIN` 20; label centred above at `79 - 14 - ink_h`; reading below at `y=102`, `"%d%%"` left and a `48.0M / 48.0M` detail right, in 14 px. **Keep the percent gate**: redraw only when the whole percentage changes. That gate is what stops the bar being slower than the write it reports on, and in a pipe filter fed 64 KB at a time it is the difference between 750 redraws and 100.

**Not ported:** wallpaper (there is none), softkey bar (recovery has no softkeys), `nd_fit_font`'s ladder (two sizes, chosen per call site), ellipsis-vs-step-down (14 px + hard truncation at the box edge).

---

## 7. What is deliberately left out

- **No auto-repeat synthesis.** `nd_keypad.h`'s 400/120 ms repeat exists in `nd_input`; five-item menus do not need it. evdev value 2 is accepted so a held arrow on the QEMU keyboard still scrolls, which costs one `||`.
- **No T9, no text entry.** Recovery has no field to type into.
- **No `nd_err`, no `nd_log`.** `nd_types.h` and `nd_log.h` are not reachable. Errors are `int` returns and `fprintf(stderr, …)`, which lands on `/dev/console` where the shell's `log()` already writes. Section 3 of CODING-STANDARDS names a convention for code that links the library; this program is outside it, and it says so in its header comment.
- **No heap after startup.** One `mmap` of fb0, one static frame path, static arrays for the menu (16 items × 64 chars) and the keymap. `-Wvla` is on and §1.5 forbids input-sized arrays; a package list longer than 16 is truncated with a note on the console.

---

## 8. `neodct/src/Makefile`

Follow the `displayd` rule shape, not the `nd-verify` one (dynamic, not static — musl `libc.so` is already in the initramfs for busybox, so a dynamic link costs zero new bytes; `nd-verify` is static only to avoid a 5.8 MB `libcrypto.so`).

```make
RECUI_SRCS := $(wildcard recovery/*.c)
RECUI_OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(RECUI_SRCS))
TARGETS    += $(if $(RECUI_SRCS),$(BINDIR)/nd-recui)

# No -Iinclude and no $(PKG_CFLAGS): this program must not be able to include
# an nd_*.h by accident. It runs in the initramfs, before libneodct exists.
# Full warning set including -Wconversion -- unlike displayd, this is new code.
$(OBJDIR)/recovery/%.o: recovery/%.c
	$(Q)mkdir -p $(dir $@)
	$(say) CC $<
	$(Q)$(CC) $(WARNINGS) $(OPTFLAGS) $(SANFLAGS) -D_GNU_SOURCE -MMD -MP \
	    $(NEODCT_CFLAGS) $(CFLAGS) -c -o $@ $<

$(BINDIR)/nd-recui: $(RECUI_OBJS)
	$(Q)mkdir -p $(dir $@)
	$(say) LINK $@
	$(Q)$(CC) -o $@ $^ $(LDFLAGS_ALL)
```

Also: add `nd-recui` to `install-boot` beside `nd-verify` (strip it there, same reasoning — `BINARIES_DIR` is not stripped by buildroot and this goes into the kernel image on the Luckfox); add a `skipped:` line; add `recovery` to the `format:` target's `find` list. `clean` is `rm -rf $(BUILD)` and needs nothing. **Do not add it to `install:`** — it has no caller in the running system and would be dead weight in the verity-covered squashfs, which is the same argument the Makefile already makes for `nd-verify` at `BOOTDESTDIR`.

---

## 9. `neodct/scripts/mkinitramfs.py`

```python
RECUI_CANDIDATES = ("NeoDCT/System/bin/nd-recui", "usr/bin/nd-recui", "bin/nd-recui")
```

- add `--recui PATH` to `main()`, mirroring `--verifier`
- add `find_recui(target_dir, recui)` mirroring `find_verifier()`
- in `build()`, inside the `extra_binaries is None` branch, after the panel-daemon block: if found, `binaries["bin/nd-recui"] = found`, gated on the **same `elf_machine()` check the panel daemon gets**. `install-boot` writes into `BINARIES_DIR`, which is not architecture-tagged, so a stale cross build from another board is a real way to ship an unexecutable binary — and that check is exactly what the existing comment says it is there to catch.
- **optional, warn on stderr, do not fail the build** — the panel-daemon precedent, not the `dmsetup`/`nd-verify` one. An initramfs without `nd-recui` still recovers a phone: the shell falls back to the tty menu, which is today's behaviour. Failing the build over a nicer menu would be wrong. (It is close: on hardware, without it, recovery has no working input at all. But that is *already* true today, so its absence is a regression to the status quo, not a broken image.)

`neodct/scripts/post-image-neodct.sh` gains one line: `--recui "$BINARIES_DIR/nd-recui" \`.

No new `APPLETS` entries. No new `SPLASH_IMAGES`. No new `plain_files`.

---

## 10. `neodct/initramfs/ndsys-recovery.sh`

Small and surgical. The tty menu stays as-is and stays the fallback.

```sh
: "${RECUI_BIN:=/bin/nd-recui}"
: "${RECOVERY_KEYMAP:=$MNT_USER/keymap.json}"
```

Call it through the variable, exactly as `nd-verify` is called through `${NDSYS_VERIFY_BIN:-…}`. Two payoffs: `test_initramfs_applets.py`'s scanner cannot see a variable, so `EXTRA_BINARIES` needs no edit; and the host tests can substitute a stand-in. (The alternative — a literal `nd-recui` plus `EXTRA_BINARIES |= {"nd-recui"}` — also works; the variable is better and has precedent.)

One predicate:

```sh
# The panel path is available when the binary shipped, fb0 came up, and
# nobody asked for the serial console instead. neodct.rectty=/dev/console
# is a deliberate request for a text UI on a cable; honour it.
recovery_panel_ui() {
    [ -z "${RECOVERY_TTY_OVERRIDE:-}" ] || return 1
    [ -x "$RECUI_BIN" ] || return 1
    [ -n "$PANEL_UP" ] || return 1
    return 0
}
```

`PANEL_UP` is the right flag: `panel_start()`'s own comment says returning 0 means "`/dev/fb0` is there and worth drawing on", which is true on QEMU with no daemon at all.

Then:

- `recovery_menu()` — first line: `if recovery_panel_ui; then "$RECUI_BIN" menu --keymap "$RECOVERY_KEYMAP" "$@"; rc=$?; [ "$rc" != 2 ] && return 0; fi`, then the existing body unchanged.
- `recovery_confirm()` — same, delegating to `confirm`.
- The seven inline `screen_clear; say; say; …; read_key > /dev/null` blocks in the action functions become `recovery_say` — one new helper that delegates to `nd-recui message` or falls back to the existing sequence. This is the only change that touches more than a few lines.
- `recovery_install_package()` gains the three `| "$RECUI_BIN" progress …|` stages, each guarded so that without the binary the pipelines are byte-identical to today's. The cleanest spelling is a `recovery_meter()` helper that is `cat` when the panel UI is unavailable.
- `recovery_main()` — call `recovery_input_start` **only when `recovery_panel_ui` is false**, and around the `shell` option. Two readers of the same VT would leave stale bytes queued, and (worse) the VT's own echo would paint console text over the framebuffer. `nd-recui`'s `KD_GRAPHICS` handles the echo; not opening fd 8 handles the stale bytes.
- Keep `printf '\033[?25l'` on the cursor.

`init` needs **no change**. The sad face already appears; `panel_start` already runs; `panel_stop` before `switch_root` is already there.

---

## 11. Tests

- **`neodct/src/test/unit/test_recui.c`** — the pure functions, which is where the value is (SKILL.md: "Pure functions — a key map, a layout, a formatter — are worth far more than UI tests here"). Cases: the keymap extractor against a real `nd_keymap_save()` payload and against a hand-mangled one; the name→code table walking **only the sixteen real codes**, which is the check that catches a future edit routing something onto Left/Right; the list windowing arithmetic against `nd_vlist`'s (row y at 40/73/106, `item_height` 29, `selected_right` 225); the percent gate; glyph advance sums against `fontref.json`'s `sum_of_advances` for the strings it already records at 14 and 18 px. The matrix scan is driven through a `socketpair` standing in for the chip — the exact hook `nd_keypad.h` documents and `test_keypad.c` already uses.
- **`neodct/tests/test_recui_font.py`** — regenerate `nd_recfont.h` and byte-compare; `pytest.importorskip("PIL")`.
- **`neodct/tests/test_mkinitramfs.py`** — three new cases mirroring the existing panel-daemon trio: it ships when it matches, it is left out with a warning when the architecture differs, a missing one does not fail the build.
- **`neodct/tests/test_initramfs_recovery.py`** — unchanged and must stay passing. Every existing case runs with no `$RECUI_BIN` on disk, so every delegation falls through to the code those tests already pin. That is the regression net for this change and it is worth stating in the commit.
- **Not** a golden frame. CODING-STANDARDS §7: "Do not cut a *new* frame for a *new* screen." And `nd-shoot` links libneodct, so it could not render these anyway.

Full gate: `cd neodct/src && make && make test` and `make ASAN=1 test`; `python3 -m pytest neodct/tests/ -q` (12 pre-existing failures; skip `test_bluetooth.py` and the four `test_mediawidget*.py`).

---

## 12. Byte budget

Measured from the built image:

| | uncompressed | in `initramfs.cpio.gz` |
| --- | --- | --- |
| initramfs today | 7,541,451 B | 3,713,161 B |
| `neodct_displayd` (the size reference) | 18,008 B | 7,960 B |
| one 240×175 raw asset | 168,000 B | ~615 B |

**Budget: `nd-recui` ≤ 40 KiB stripped uncompressed, ≤ 16 KiB added to `initramfs.cpio.gz`. Hard fail above 48 KiB uncompressed.** Of that, 5,126 B is the font table, which is incompressible; the rest is code and should land near `neodct_displayd`'s 18 KiB. Zero new shared libraries (musl `libc.so` is already there for busybox), zero new busybox applets, zero new raw assets.

The comparison that decides the font: **one pre-rendered screen costs 168,000 bytes uncompressed — five times the entire binary, thirty-three times the font** — and the cpio is unpacked into RAM on a 64 MB device, where uncompressed is the number that matters.

---

## 13. What I could not verify, and the one real risk

**`/dev/i2c-3` must exist in the initramfs, and I cannot prove it does.** The Luckfox kernel is built by the RV1103 SDK outside this tree — there is no kernel config in the repo to check. `CONFIG_I2C_CHARDEV` must be **`=y`, not `=m`**: modules live in the rootfs, which is precisely the filesystem recovery exists because it cannot mount, and there is no `/lib/modules` in the initramfs for `modprobe` to find. If it is `=m`, `nd-recui` will find no bus, fall through to evdev, find nothing there either, and exit 2 — recovery degrades to today's serial-only behaviour rather than breaking, which is the right failure but is not the goal. **This needs one `ls /dev/i2c-*` at a recovery prompt on real hardware before the work is called done**, and it is the single thing in this plan that a host cannot answer.

Two smaller ones: the ST7789 mirror path is only exercised on hardware (`neodct_displayd` exits immediately on QEMU, where fb0 is the screen); and whether 500 µs of settle is enough for the phone's membrane is already noted as unverifiable in `nd_keypadsetup.c` and stays so.

---

## 14. Sequencing

1. `mkrecfont.py` + `nd_recfont.h` + `test_recui_font.py` — measurable, independent, no build changes.
2. `nd_recdraw.c` + the `splash` and `message` verbs. Verify against the existing `/splash.raw` under QEMU.
3. `nd_recinput.c`, evdev only. QEMU-drivable end to end at this point.
4. The i2c backend and the keymap reader, with the socketpair test.
5. `menu` and `confirm`, then the `progress` filter.
6. Makefile, `mkinitramfs.py`, `post-image-neodct.sh`, `ndsys-recovery.sh`.
7. Full test gate, then hardware.

---

## Housekeeping

**Nothing was committed, and there is nothing to commit.** The deliverable is a plan and the task says explicitly not to write code in this phase; the subagent instructions say not to write findings to a `.md` file. The plan is this message.

**The worktree premise is also wrong.** `git worktree list` returns a single entry — this session is in the **main checkout at `/home/user/neodct`, on branch `0.5.0b-dev`**, two commits ahead of `origin/0.5.0b-dev`, working tree clean and untouched. There is no linked worktree to report. If the implementation phase should happen on a branch, one needs creating first.

# Testing system updates by hand

How to build an `UPDATE.ndsw`, install it on a running phone, and reproduce
every refusal on purpose.

Every case below is also asserted automatically in
`neodct/tests/test_mkbadupdate.py` — that suite checks each broken package
still triggers its specific error in the same `UpdateService` code the phone
runs. What it cannot check is what appears on the screen, which is what this
document is for.

---

## 1. Build a good update

```sh
cd buildroot
make neodct_qemu_defconfig      # first time only; overwrites .config
make                            # produces the whole image set
NEODCT_SIGN_KEY=$PWD/../neodct/tools/devkey/neodct-dev.key make update
```

That writes `output/images/UPDATE.ndsw`. Without `NEODCT_SIGN_KEY` it still
builds, just unsigned — which is the BAD SIGNATURE path, useful on purpose.

For the update to be *visible* as a new version, bump the version first:
`VERSION_ID` in `neodct/overlay/etc/os-release` is the single source of
truth. Change `0.3.0a` to `0.3.1a`, `make`, then `make update`. Otherwise
you install the same version you are already running and nothing on screen
changes.

A release can carry a picture: drop a square PNG (64x64 is the size the
phone draws) at `neodct/release-thumbnail.png` and `make update` puts it in
the package, or pass `NEODCT_UPDATE_THUMBNAIL=/path/to/art.png`. Its sha256
goes into `manifest.json`, so the one signature covers the art too; a
picture that does not match is dropped and the stock icon is drawn instead.

Check what you built:

```sh
unzip -l output/images/UPDATE.ndsw
unzip -p output/images/UPDATE.ndsw manifest.json
```

## 2. Put it on the card

**Option A — write into the card image** (QEMU must not be running):

```sh
neodct/tools/sdcard.sh put buildroot/output/images/UPDATE.ndsw
neodct/tools/sdcard.sh ls update
```

**Option B — a host folder as the card** (drag and drop while it runs):

```sh
NEODCT_SD=share neodct/tools/run_qemu.sh
# then just copy into ~/neodct-sdcard/update/
```

Note the card *detection* and *format* flows cannot be tested this way — a
virtiofs share is not a block device and has no FAT label. Use option A for
those.

**On real hardware:** a FAT32 card with `wallpapers/ tones/ backup_db/
music/ update/` at the top level; drop the file in `update/`.

## 3. Install it

```sh
neodct/tools/run_qemu.sh        # do NOT use NEODCT_SNAPSHOT here
```

`NEODCT_SNAPSHOT=1` discards all writes on exit, so the staged update
disappears and the install silently appears to do nothing.

On the phone: **Update** (app 12) → it finds `UPDATE.ndsw` → one page
showing the release picture, version, size, build date, whether it is signed
and the release notes (**up/down** scrolls the notes) → **Install** → one
progress screen that backs up contacts and messages to the card and then
copies the image → **Ready**, softkey **Restart**.

Nothing asks about the backup: it happens on the way past, and if the card
will not take it the install carries on and says so on the last page (user
data is on its own partition and an update never touches it).

The phone reboots. The initramfs prints `[ndsys]` lines to the serial
console as it verifies and writes the image, then boots into it.

## 4. Confirm it actually installed

- Settings → About shows the new version (this comes from
  `/NeoDCT/System/version.prop` inside the new image, so if it changed, the
  new rootfs really is what booted).
- Opening Update again reports **Updated / NeoDCT 0.3.1a** once, then
  forgets it.
- On the serial console: `[ndsys] installed 0.3.1a`.
- Full history: `/NeoDCT/User/logs/update.log`.

---

## 5. Break it on purpose

```sh
neodct/tools/mkbadupdate.py --list      # what is available

neodct/tools/mkbadupdate.py \
    buildroot/output/images/UPDATE.ndsw \
    --output-dir /tmp/bad \
    --key neodct/tools/devkey/neodct-dev.key
```

That writes one `.ndsw` per variant into `/tmp/bad`. Copy one at a time onto
the card as `UPDATE.ndsw` (the app prefers that name, and it is less
confusing than having several):

```sh
cp /tmp/bad/unsigned.ndsw /tmp/UPDATE.ndsw
neodct/tools/sdcard.sh rm update/UPDATE.ndsw
neodct/tools/sdcard.sh put /tmp/UPDATE.ndsw
```

Variants that edit the manifest are re-signed with `--key`, so they fail for
exactly one reason instead of also tripping the signature check. Without a
key those variants are skipped rather than written misleadingly.

### What you should see

| Variant | Expected on screen | Bypass? |
|---|---|---|
| `unsigned` | `BAD SIGNATURE! UPDATE MAY BE CORRUPT!!` softkey **OK**, then `Install Anyway?` softkey **OK** | Yes, engineering mode only, **and only as far as the reboot** — see below. **C** at the second dialog cancels |
| `wrong-key` | same as above | Yes, engineering mode, same caveat |
| `tampered-manifest` | same as above (version reads `…-tampered`) | Yes, engineering mode, same caveat |
| `no-manifest` | `INVALID UPDATE! UPDATE MAY BE CORRUPT!!` softkey **OK** | No |
| `no-image` | same | No |
| `not-a-zip` | same | No |
| `corrupt-image` | installs as far as the **progress bar**, then `INVALID UPDATE! UPDATE MAY BE CORRUPT!!` | No |
| `truncated-image` | same as `corrupt-image` | No |
| `wrong-platform` | `WRONG UPDATE FOR THIS PHONE!` + `update is for luckfox-armv7, this is qemu-aarch64` | No |
| `future-kernel` | `WRONG UPDATE FOR THIS PHONE!` + `update needs kernel 99.0.0, running 6.12.47` | No |
| `bad-root-hash` | **installs with no complaint**, then fails to boot — see below | No |

Two things worth understanding from that table:

**`corrupt-image` gets past the signature check.** The signature covers
`manifest.json`, and the manifest's `sha256` covers the image — so a flipped
byte in the image is only caught when the image is hashed during the copy.
That is by design: it means you see the progress bar first. It is caught
again by the initramfs before anything is written to the system partition,
and a third time by dm-verity on every boot afterwards.

**Engineering mode gets an unsigned package staged, not installed.** The
initramfs checks the release signature itself before it writes anything, and
it does not read engineering mode — that setting lives in `settings.prop` on
the writable partition, which is exactly the partition the check exists to
stop trusting (`SECURITY-AUDIT.md` section 3). So the three engineering-mode
rows above stage, reboot, and come back with the old system and
`last_result.prop` reading `not signed by the release key`. To carry one all
the way through, boot with the escape hatch:

```sh
NEODCT_UNSIGNED=1 neodct/tools/run_qemu.sh
```

which puts `neodct.unsigned=1` on the kernel cmdline. That is the U-Boot
environment on a real phone, so it is not something a running system can set
for itself — which is the whole reason the hatch is there and not in a
setting. The applier logs a `WARNING` line naming it on every boot it is used.

**Turn engineering mode off** (Settings → Engineering Mode) and re-run the
`unsigned` case: the dialog must become `BAD SIGNATURE! UPDATE MAY BE
CORRUPT!!` with an **OK** softkey and no way through. If a non-engineering
build can install an unsigned update, that is a bug.

Also worth trying by hand, since no automated test covers the screens:

- **No card at all** — `NEODCT_SD=none run_qemu.sh` → "No SD card." then an
  explanation of the folder layout.
- **A card that is not ours** — `neodct/tools/sdcard.sh new 128` then delete
  the folders, or attach any FAT32 image → Settings → Memory card offers
  "Set up" (non-destructive). An unreadable card offers "Format" and warns
  `EVERYTHING ON IT WILL BE ERASED!`.
- **Empty `update/` folder** → "No update found." plus instructions.

---

## 6. Recovery mode

Three ways in:

```sh
NEODCT_RECOVERY=1 neodct/tools/run_qemu.sh      # boot straight into it
```

From a running phone, leave a one-shot flag and reboot -- this is the route
that also works on real hardware, where you cannot edit the kernel cmdline:

```sh
touch /NeoDCT/User/.ndsys/boot_recovery && reboot
```

And automatically, whenever the phone cannot boot: no system image found,
dm-verity refused it, the root would not mount, or no `/sbin/init`. That is
the case recovery exists for.

The menu is drawn on the phone's screen (the framebuffer console), so in QEMU
it appears in the display window -- click into it for keyboard focus. Arrow
keys or `j`/`k` move, Enter selects, and the destructive choices default to
"no". If keys are not reaching the VT, drive it over the serial console
instead:

```sh
NEODCT_RECOVERY=1 NEODCT_RECTTY=/dev/console neodct/tools/run_qemu.sh
```

    NeoDCT recovery
    > update system
      wipe user data
      wipe system
      reboot
      shell

**update system** mounts the card, lists `*.ndsw`, hashes the image straight
out of the zip *before* writing anything, writes it, reads it back, then
records what is installed and reboots. It checks the release signature and
tells you the answer -- "Signed by the release key" or "is NOT SIGNED.
Install it anyway?" -- but it does not refuse over it. Recovery exists to
rescue a phone that will not boot, with a person standing in front of it, and
an owner whose only image is an unsigned development build still has to be
able to get it running. The automatic applier is the one that refuses.

**wipe user data** erases contacts, messages and settings but keeps
`.ndsys`: deleting that would take `installed.prop` with it, leaving the next
boot with no root hash to verify against and landing straight back here.

**wipe system** zeroes the first megabyte of the system partition, which is
enough to make it unbootable. Use it to test that the automatic entry into
recovery works.

The flag is consumed as it is read, so rebooting out of recovery returns to
the normal system rather than looping.

---

## 7. Prove dm-verity is really enforcing

The most direct check needs no update package at all. Corrupt the installed
system image on the host and boot it:

```sh
cd buildroot/output/images
cp system.img system.img.good
# Flip a byte a few MB in, well inside the filesystem data.
printf '\xff' | dd of=system.img bs=1 seek=4000000 conv=notrunc
cd -
neodct/tools/run_qemu.sh
```

With verity working, that block fails its hash check when something reads
it: you get I/O errors, a failed boot, or a rescue shell — never a silently
running system with corrupt data. Put it back with
`cp system.img.good system.img`.

The `bad-root-hash` variant tests the same guarantee from the other
direction: the manifest claims a root hash that does not match the image, so
it passes every check the app makes, installs, and only fails at boot. The
serial console shows either `dm-verity table load failed` or `cannot mount
the system image`, depending on when the kernel notices, and then drops to a
rescue shell rather than a reboot loop.

To recover from that:

```sh
NEODCT_VERITY=permissive neodct/tools/run_qemu.sh   # boots unverified, warns
# then install a good UPDATE.ndsw normally and reboot
```

`NEODCT_VERITY=off` skips verity entirely if you need to get in and look
around.

---

## 8. Power-fail safety

The one property that is hard to test by hand: the applier writes the image,
reads it back off the device, hashes it, and only *then* deletes the staged
copy. Kill QEMU while the `[ndsys] installing …` line is on the console and
boot again — it should install cleanly on the retry, because the pending
image and its record are both still there. After three failed attempts it
gives up, records the failure, and boots the old system.

`neodct/tests/test_initramfs_apply.py` covers this with a fake device,
including the give-up path.

# Security audit — the phone as it stands before 0.5.0

Companion to `SECURITY.md`, which is a **design conversation about a future**.
This is a **measurement of the present**: what the code in this tree actually
does, with the line that does it. Where the two disagree, this file is right
and `SECURITY.md` has been corrected.

Scope: `neodct/src` (the C build), `neodct/overlay`, `neodct/initramfs`,
`netsurf-neodct`, and the two defconfigs. Read against `main` at `79367c3d`,
which carries `VERSION_ID=0.4.3a` with the sleepy and clock work held as
Unreleased; 0.4.5 development builds are materially the same for everything
below. **Every finding here is architectural** — none of it turns on a version
number, and none of it has been fixed by anything between 0.4.3a and now. Line
numbers are the ones to re-check after a big refactor, not the findings.

---

**Since this was written, much of it has been fixed.** `SECURITY-PLAN.md`
section 7 records exactly what, and the findings table in section 9 below
carries a status column. This document is still a measurement of the state it
was taken in and has NOT been rewritten to describe the present — a
measurement that gets edited to match later work stops being evidence of
anything. Where a section has been addressed it is marked in place, with what
changed and what is left; where it has not, the finding stands as written.

---

## 0. The one-paragraph verdict

The **process architecture is right** and is the hard part — apps are separate
processes reached through one `fork()`+`execve()` chokepoint, which is the only
place a kernel can be taught to care. What is missing is anything that uses it.
There is no confinement of any kind in the image: no SELinux, no Landlock, no
seccomp, no capability drop, no user other than root, and no permission field in
any manifest. Every process on the phone has every permission, so **the security
boundary between an app and the phone is currently the app's own good manners.**

The second finding is more specific and more urgent than the general one:
**dm-verity is not currently an anti-persistence measure**, because its root hash
is read from the writable partition and the initramfs applier checks no
signature. `SECURITY.md` claimed the phone "cannot be permanently backdoored
through the update path". That claim was wrong and has been removed.

> **Since fixed.** The initramfs carries its own verifier and the release key,
> checks `manifest.sig` over `manifest.json` before anything reaches `dd`, and
> then compares every field of `pending.prop` against the signed manifest —
> because the attack is a genuine package with a rewritten record. The verdict
> above holds for everything else in this section: two users now exist and the
> browser is one of them, but nd-core is still root and there is still no
> permission field.

---

## 1. What is already right

Credit where it is due, because these are the expensive parts and they are done.

| Property | Where |
| --- | --- |
| `/` is read-only squashfs; no `remount,rw` anywhere in boot | `overlay/etc/inittab:4-7`, `overlay/etc/fstab:3-7` |
| Apps are separate processes, not `dlopen`ed into the core | `apprun/nd_apprun.c`, `include/nd_app.h` |
| **One** app-launch call site, ready for `setexeccon`/Landlock | `lib/nd_proc.c:305` (`nd_proc_spawn`), `nd_proc_launch_app` |
| `nd-apprun` already has an "open everything, then load the app" order | `apprun/nd_apprun.c` — the numbered list in its header |
| Apps get the framebuffer as an **inherited fd**, so they need no `/dev/fb0` | `ND_ENV_FB_FD`, `lib/nd_app.c` |
| The app→core service wire is deliberately four operations, no raw AT | `include/nd_svc.h` |
| The media player is already a **separate process** | `lib/nd_media.c:650` |
| sshd binds `127.0.0.1`, key-auth only, `PermitRootLogin prohibit-password` | `lib/nd_remoteshell.c:706` |
| Update signature is RSA/SHA-256 over a manifest that hashes everything else | `include/nd_package.h:11` |
| Unsigned update override is gated behind engineering mode | `apps/Update/main.c:921` |
| The initramfs parses records with `sed -n s///p`, never by sourcing — so a record value can never execute | `initramfs/ndsys-apply.sh:174-178` |
| NetSurf is built with JavaScript **off** on both targets | `buildroot/package/netsurf/netsurf.mk:109` |

Items 3 and 4 are exactly what `SECURITY.md` asked the port to preserve, and the
port preserved them. Everything in section 6 below is *additive*; none of it
requires undoing work.

---

## 2. What is not

### 2.1 There is no confinement primitive in the image at all

> **Partly addressed.** There are now two users, a mode-bit layout on the
> writable partition, `no_new_privs`, and a mount namespace with hidden paths
> for untrusted children — `nd_priv.c`, `nd_proc.c`, `S00userdata`. The kernel
> fragment names `SQUASHFS_XATTR`, `MNT_NS` and `SECCOMP_FILTER`. No seccomp
> filter is installed yet and there is no MAC.

```
$ grep -rn "setexeccon|landlock|seccomp|prctl|setuid|setgid|capset|unshare|CLONE_NEW" \
      neodct/src/ neodct/overlay/
(no matches)
```

The kernel fragment (`buildroot/board/qemu/aarch64-virt/linux.config`, 171 lines)
names `CONFIG_DM_VERITY` and no other security option. Nothing enables
`SECURITY_LANDLOCK`, nothing sets `lsm=`, and `mksquashfs` is not asked for
xattrs — so even if a policy existed there is nowhere to put a label.

### 2.2 Everything runs as root, including things that need almost nothing

> **Partly addressed.** netsurf-fb and everything it starts run as `ndusr_ut`,
> which is not in `dialout` and not in group `ndusr`. nd-core, nd-apprun and
> the apps are still root; `SECURITY-PLAN.md` section 7 says what stops the
> core dropping.

`inittab` runs `run_neodct.sh` on tty1, which execs `nd-core`, which forks
`nd-apprun`, which `dlopen`s app code. Every one of those is uid 0. The
Calculator has `CAP_SYS_ADMIN`.

### 2.3 There is no app permission model, because there is no permission field

An app manifest is four keys:

```json
{ "name": "Downgrade", "id": "9006", "icon": "icon.png", "exec": "main.py" }
```

`nd_ui_scan_apps()` (`lib/nd_ui.c:532`) reads `name`, `id`, `icon`, `exec` and
nothing else. There is no field for what an app may touch, and therefore nothing
for a future `setexeccon()` to read.

### 2.4 The writable partition is mounted with no restrictions

> **Fixed.** `nosuid,nodev` on the user partition, `/tmp`, `/dev/shm`, `/run`
> and every card mount; `noexec` on the applier's card mount and on the card's
> arrival partition. Not on `/tmp` or the user partition, deliberately: noexec
> refuses `mmap(PROT_EXEC)` as well as `execve`, so it would break nd-apprun's
> own `dlopen()` of `app.so`.

```
initramfs/init:170     mount -t ext4 -o rw,noatime  $USER_DEV /mnt/user
System/hw/neodct-sdcard:178   options="rw,noatime"
overlay/etc/fstab       tmpfs /tmp  mode=1777      (no nosuid/nodev/noexec)
```

No `nosuid`, no `nodev`, no `noexec`, on the user partition, on `/tmp`, on
`/dev/shm`, or on any SD card — and cards are **auto-mounted on insertion** by
`60-neodct-sdcard.rules`. Today this changes nothing, because everything is
already root. The moment a `ndusr` exists it becomes a one-line privilege
escalation from a card someone plugs in, so it is worth fixing *before* the user
exists rather than after.

### 2.5 The browser has the whole filesystem

> **Partly addressed.** The browser runs as `ndusr_ut`, its `HOME` moved off
> the root of the writable partition into `/NeoDCT/User/browser`, and a mount
> namespace gives it an empty view of `/NeoDCT/System/engineering` and the
> rest of the list in `apps/Browser/browser.h`. The `file` fetcher is still
> built, and the rest of `/NeoDCT/System` is still readable — it has to be, it
> is where the home page lives.

NetSurf is built with the `file` fetcher unconditionally
(`netsurf/content/fetchers/Makefile:14`), so `file:///` enumerates the rootfs,
`/NeoDCT/User/db/sms_inbox.db`, `/NeoDCT/User/.remote/id_ed25519` and everything
else. This is the user's own observation and it is correct.

### 2.6 Untrusted bytes are parsed in the core process

`nd_modem__handle_urc()` and `nd_modem__parse_sms_records()`
(`lib/nd_modem.c:436`, `:1254`) parse attacker-supplied SMS text on the core's
modem thread. Text mode is a far smaller surface than PDU or MMS, so this is a
note rather than an alarm — but it is `SECURITY.md`'s rule 6 already being bent,
and MMS is what would break it.

The SNTP client added in the clock work (`lib/nd_clock.c:403`) parses network
bytes on a detached thread in the core too. Its parsing is careful — a fixed
48-byte `recvfrom`, a short reply rejected, the timestamp bounds-checked against
`ND_CLOCK_SANE_MIN`/`MAX` before it is narrowed — so there is no memory-safety
concern here. What it does not do is match the reply against the request's own
origin timestamp, so **anything that can answer first can set the phone's clock**
to any point in 2020–2100. That matters less for the clock than for what reads
it: TLS certificate validity windows in the browser and in the update
downloader. Cheap fix, and the right time to make it is while the code is new.

---

## 3. THE ONE THAT MATTERS: dm-verity is not anti-persistence

This is the finding to act on first, because it invalidates the property the
whole storage design was built to provide.

### The chain

`initramfs/init:191` calls `apply_pending` **before** verity is set up.
`apply_pending` (`ndsys-apply.sh:302`) reads its instructions from
`$STATE_DIR/pending.prop`, where `STATE_DIR="$MNT_USER/.ndsys"`
(`initramfs/init:38`) — **on the writable partition**.

What it validates:

- the staged image's size matches `image_bytes` in the record — *from the record*
- the staged image's sha256 matches `sha256` in the record — *from the record*

What it does not validate: **anything at all about who produced either.** There is
no `openssl`, no signature check, no key. The applier's own comment is honest
about why —

> *"The running system checked the package's signature before recording it; this
> checks that the bytes about to be written are the ones that was said about."*

— but that places the entire trust decision in a userspace app, and writes its
output to storage that any root process can rewrite. It then `dd`s the image over
`$SYS_DEV` and writes the attacker's own verity parameters into `installed.prop`:

```sh
putprop_file "$STATE_DIR/installed.prop" <<EOF
verity_root_hash=$(getprop verity_root_hash "$PENDING")
verity_salt=$(getprop verity_salt "$PENDING")
...
```

On the next boot `verity_table()` (`ndsys-apply.sh:281`) builds the dm-verity
table **from that file**. Verity then passes, correctly, because verity's job is
to prove the image matches the recorded hash — and the attacker recorded the
hash.

`initramfs/init:192-196` already says this in as many words:

> *"The root hash comes from installed.prop on the user partition, so this is an
> integrity guarantee rather than an authenticity one."*

That comment is right. `SECURITY.md`'s claim that the phone "cannot be
permanently backdoored through the update path" was not, and is now removed.

### What it costs an attacker

Write two files under `/NeoDCT/User/.ndsys/` and reboot. The 8 MiB user
partition on the Luckfox is too small for a 51 MiB image, but the `package=`
branch installs **straight off the SD card**, and a rogue app can write a card.

> **Fixed, by (1).** `nd-verify` — a statically linked RSA verifier built from
> the same `nd_signing.c` the Update app uses — and the release public key are
> packed into the initramfs by `mkinitramfs.py`, which fails the build without
> either of them exactly as it fails without `dmsetup`. `apply_pending` checks
> `manifest.sig` over `manifest.json` before anything reaches `dd`.
>
> The signature alone would not have been enough, and that is worth recording:
> the attack in this section is a GENUINE package with a rewritten record, so
> that the applier writes a real image and then records an attacker's root hash
> to verify against forever. So every field the signature covers — `sha256`,
> `version`, `platform`, `buildtime` and all four verity parameters — is
> compared against the signed manifest, and a mismatch is a refusal.
>
> (2) is still the better fix and is still not done: it needs the release
> certificate in the kernel keyring and `keyctl` in the initramfs. The note
> below about a kernel not shipping in an `.ndsw` is why it should land before
> the fleet grows.

### The fix, in increasing order of goodness

1. **Verify the signature in the initramfs, before the `dd`.** The public key is
   in the initramfs, which is built into the kernel image and reflash-only. This
   closes the hole with `openssl dgst -verify` and no architecture change.
2. **`CONFIG_DM_VERITY_VERIFY_ROOTHASH_SIG`** with the release certificate built
   into the kernel — the kernel then refuses a root hash nobody signed, and
   `installed.prop` stops being a trust input. The init script already names this
   as "the upgrade path"; it is the right one.
3. Both. (2) is the real fix; (1) is what ships this month.

Note that (2) needs a kernel rebuild, and per `AGENTS.md` **a kernel cannot ship
in an `.ndsw`** — so it needs to land before the fleet grows, not after.

---

## 4. The six questions, answered

### Q1 — Can a rogue app invoke AT commands?

**Yes, trivially, and it does not even need libneodct.**

> **Still yes for an app; no longer for the browser.** `ndusr_ut` is not in
> group `dialout`, so `open("/dev/ttyUSB2")` from netsurf or anything it starts
> is `EACCES`. An app is still root, so nothing changed there — nd-apprun does
> not drop, and the permission field this section asks for is Phase 3.

`open("/dev/ttyUSB2", O_RDWR)` then `write(fd, "ATD1900555xxxx;\r", 16)`. The
core takes `flock(LOCK_EX)` on the port (`lib/nd_modem_at.c:291`) but **flock is
advisory** — its own header explains it is there to cooperate with the boot
script and `atcmd`, not to exclude anyone. A tty can be opened by any number of
processes; nothing consults the lock unless it chooses to.

Separately, `libneodct.so` exports `nd_modem_open()`, `nd_modem_send_at()`,
`nd_modem_dial()` and `nd_modem_hangup()` with default visibility and no version
script, so an app can call the real thing with two lines.

The narrow four-operation `nd_svc` wire is a **convenience API, not a boundary.**
It is correctly designed to become one, once something below it enforces it.

### Q2 — Can a rogue app send messages on your behalf?

**Yes, three ways.** Via `nd_svc_sms_send()`, which is a supported call it is
*entitled* to make; via raw `AT+CMGS` on the tty as above; and it can read
`/NeoDCT/User/db/phonebook.db` for the recipient list first, since it is an
ordinary SQLite file with no access control.

### Q3 — Can a rogue app `rm -rf` your entire user folder?

**Yes.** `/NeoDCT/User` is `rw` and the app is root.

> **Still yes for an app; no for the browser.** `/NeoDCT/User` is now 0751
> `ndusr:ndusr` and the only thing `ndusr_ut` may write is
> `/NeoDCT/User/browser`. The blast radius of a NetSurf RCE is that one
> directory. An app still runs as root and this answer is unchanged for it. Contacts, messages, call
log, wallpaper, settings, ssh keys — one `nftw()` call, or `unlink()` in a loop
so it doesn't even need to `execve` anything.

Worth being precise about the blast radius, because it is the *good* news here:
this destroys user data and nothing else. The rootfs is not writable, so the
phone still boots, and `S00userdata` recreates the directory tree. It is a data
loss bug, not a brick.

### Q4 — Can a rogue app corrupt the system?

**Yes, and permanently — via section 3.** It cannot write `/` directly (squashfs,
read-only, and on the Luckfox `ubiblock0_0` is read-only in the kernel), which is
the protection working as designed. But it does not need to: it stages an update
and lets the initramfs write the partition for it, with the kernel's own
privileges, and records a verity root hash that makes the result verify cleanly
forever after.

There is a lesser version too: `settings.prop` is a plain file on the writable
partition, so an app can set `system.ui.engineering_mode=1` behind the user's
back. That unlocks LinuxShell, raw-AT Modem, and **Downgrade** — rollback to an
older *properly signed* image with known bugs, which no signature check will ever
refuse.

### Q5 — Can a rogue app create a persistent payload?

**Yes. This is the one you guessed was a "no", and it is the most emphatic yes of
the six.** dm-verity protects the rootfs; it does nothing for the writable
partition, and boot reads code and configuration from that partition in at least
five places.

| # | Vector | Mechanism | Severity |
| --- | --- | --- | --- |
| 1 | `/NeoDCT/User/.remote/state.prop` | Write `enabled=1`, `host=attacker.vps`, drop an `id_ed25519`. `nd_main.c:367` calls `nd_rs_start_if_enabled()` on every boot; the phone opens a reverse-SSH tunnel out to the attacker with a root shell on the far end. Outbound over mobile data, so no inbound firewall matters. **Survives OTA updates**, because an update replaces only the rootfs. | **Critical** |
| 2 | ~~`/NeoDCT/User/env.sh`~~ | **Closed.** Still sourced, but only when the kernel cmdline says `neodct.devenv=1` or the verity-covered rootfs carries `/etc/neodct-devenv` — neither of which a running system can write. Engineering mode is deliberately not accepted: it lives on the partition the attacker just wrote to. | ~~Critical~~ |
| 3 | ~~`/NeoDCT/User/.ndsys/pending.prop`~~ | **Closed.** The initramfs verifies the release signature and compares every field of the record against the signed manifest before anything reaches `dd`. A forged record is a refusal; an unsigned image needs `neodct.unsigned=1` on the kernel cmdline. | ~~Critical~~ |
| 4 | `/NeoDCT/User/.remote/authorized_keys` | Add a key; it is honoured whenever sshd runs. Also loadable **from an SD card** (`nd_rs_card_file_names`, `lib/nd_remoteshell.c:111`) — so this one is reachable with physical access and no code execution at all. Narrowed, not closed: `.remote` is 0700 `ndusr` and empty inside the browser's namespace, and the card path is the MEDIA partition, which the untrusted set cannot write. An app is still root. | High |
| 5 | `/NeoDCT/User/settings.prop`, `keymap.json`, the databases | Not code execution, but permanent behavioural change the user cannot see. | Medium |

Vector 2 is worth dwelling on: it is not a bug, it is a documented developer
convenience, and it is a good one. It just needs to stop being unconditional —
gate it on a build flag, an engineering-mode setting, or a file the *rootfs*
vouches for.

### Q6 — Does the system trust any app in `/NeoDCT/User/apps`?

**No — and this is the best news in the audit.** You were expecting worse than
reality. Nothing in the tree references `User/apps`:

```
$ grep -rn "User/apps" neodct/
(no matches)
```

`nd_ui.c:624,627` scans exactly `/NeoDCT/System/apps` and
`/NeoDCT/System/engineering/apps`, both inside the verified read-only image.
**There is no user-installable app path today.** Every app that runs is one you
shipped, and the reason apps are dangerous in this report is that they are native
root code — not that untrusted ones can currently arrive.

That is a genuinely strong position to be in. It means **the confinement work can
land before the first third-party app exists, rather than after** — which is the
opposite of the situation Android was in, and it is worth spending 0.5.0 on.

---

## 5. On splitting `libneodct.so`

Direct answer: **splitting it into `libndModem.so`, `libndClock.so` and so on
would buy you no security whatsoever.** Worth being clear about why, because the
instinct behind the question is right even though this particular expression of
it is not.

A shared library is not a boundary. It is a **mapping in an address space you
already control**. An app that is denied `libndModem.so` writes ten lines of
`termios` and `write()` and talks to `/dev/ttyUSB2` directly; it never needed the
library. Even `dlopen`ing it by hand works, and `SECURITY.md` already makes the
architectural half of this point: *"SELinux domain transitions happen only at
`execve()`. A library loaded with `dlopen()` does not change a process's security
domain."* The same is true of every mechanism — the linker is not an access
control system and cannot be made into one.

The costs of splitting are real, though:

- **RAM.** Each `.so` is a separate mapping with its own ELF headers, PLT/GOT and
  page-alignment slack — call it 8–20 KB of overhead per library, times N
  libraries, times every process. On a 64 MB phone that is a measurable regression
  in exchange for zero security.
- **Link-time dead-stripping is lost.** The Makefile's
  `-ffunction-sections -fdata-sections` + `--gc-sections` is doing exactly the job
  splitting would claim to do, and doing it per-binary and automatically. Its
  comment says so: *"keeps the unused half of libneodct out of every app's
  mapping, which is the whole point of the shared library on a 53 MB device."*
- Circular dependencies. `nd_ui` ↔ `nd_modem` ↔ `nd_notify` are not cleanly
  separable today.

**What actually compartmentalises is the kernel**, at three levels, and all three
are already available to you:

1. **The process boundary you already built.** Apps are separate processes.
2. **A policy attached at `execve()`** — Landlock/seccomp self-applied in
   `nd-apprun`, or an SELinux domain via `setexeccon()`. This is what makes "the
   Music app has no modem" true rather than merely intended.
3. **A broker.** `nd_svc.h` is already the right shape: the app has no modem
   handle, it asks the core, and the core decides. Once (2) makes the direct path
   impossible, `nd_svc` stops being a convenience and becomes *the* interface —
   and every permission question turns into "does the core agree to do this for
   this app", answered in ordinary C you can read and test.

**One split is worth making, and it is not a library split.** `SECURITY.md`'s
item 4 — the media decoder as its own process — is the exception, precisely
because it is a *process* boundary and not a linkage one. `lib/nd_media.c:650`
already forks `neodct-play` for playback; image decode for MMS should follow the
same shape before MMS exists.

---

## 6. The road to 0.5.0

Ordered by (value ÷ effort), not by ambition. Phases 0 and 1 are worth doing
regardless of how far the rest gets.

### Phase 0 — no architecture change, mostly one-liners

1. **Sign-check in the initramfs before the `dd`.** Section 3. Highest value in
   this document.
2. **`nosuid,nodev` on `/NeoDCT/User`, `/tmp`, `/dev/shm` and every SD mount**;
   `noexec` on the card mount. Four string edits.
3. **Gate `env.sh`.** Keep the feature; require engineering mode or a build flag.
4. **Enable xattrs in `mksquashfs`.** Costs nothing now, and labelling is
   impossible later without it. `SECURITY.md` item 5.
5. **Confine the browser's filesystem view.** Details in section 7.
6. Correct `SECURITY.md`'s update-path claim. *(Done in this commit.)*

### Phase 1 — stop running the UI as root

This is your own top ask and it is achievable, but the honest framing is that
**it is not a single change**, so plan it as a milestone.

Create `ndusr` with home `/NeoDCT/User`. What `nd-core` actually needs, from a
sweep of every device the tree touches:

| Need | Grant |
| --- | --- |
| `/dev/fb0` | group `video` |
| `/dev/input/event*`, `/dev/uinput` | group `input` |
| `/dev/i2c-3` (keypad, fuel gauge) | group `i2c` |
| `/dev/ttyUSB*` (modem) | group `dialout` |
| ALSA | group `audio` |
| reboot / poweroff | `CAP_SYS_BOOT` on `nd-core`, or a tiny setuid helper — **prefer the helper**, it is auditable |
| `/sys/class/backlight`, `/sys/class/gpio` (`lib/nd_backlight.c`) | write access on those attributes via a udev rule, the usual way |
| `/sys/.../cpufreq/scaling_{min,max}_freq` (`lib/nd_cpufreq.c`) | same — a udev rule, not root |
| `clock_settime` + `ioctl(RTC_SET_TIME)` for SNTP (`lib/nd_clock.c`) | `CAP_SYS_TIME` on `nd-core`, or route it through the same helper as reboot |
| spidev | none — `neodct_displayd` is already a separate process and can stay root |
| mounting SD cards | none — the udev helper already runs as root out of udev |
| sshd | none — `nd_rs_*` writes config and spawns; give that one path a helper too |

Do it in this order, because each step is separately testable:
`ndusr` exists → udev rules and group membership → `nd-core` drops privilege
after opening the framebuffer → helpers for reboot and sshd → remove the last
root need. Expect the modem and the i2c keypad to be where it bites; both are
opened early and both are easy to verify in QEMU.

### Phase 2 — Landlock + seccomp in `nd-apprun`

Kernel is 6.12.47, so Landlock is available and needs no policy files, no xattrs
and no daemon. `nd-apprun` is the ideal site and its header already documents the
right ordering. Concretely, between step 4 (inherited descriptors) and the
`dlopen` at `nd_apprun.c`:

```
open app dir, asset dir, the app's own writable dir
  → landlock_restrict_self()      ← no new paths after this point
  → seccomp filter                ← no socket(), no ptrace, no mount, no reboot
  → dlopen(app.so)                ← untrusted code starts already caged
```

Two rules earn most of the value on their own: **no filesystem write outside the
app's own directory**, and **no `open()` of `/dev/tty*`**. That alone answers Q1
and Q3 for every app that is not the Modem app.

### Phase 3 — a permission field, and a broker that reads it

Add to `manifest.json`:

```json
{ "name": "Music Player", "id": "6",
  "permissions": ["audio", "storage.media"] }
```

`nd_ui_scan_apps()` parses it into `nd_app_entry`; `nd_proc_launch_app()` turns it
into the Landlock ruleset and the seccomp filter; `nd_svc` checks it before
honouring a request. The Music app declares no `modem`, so `nd_svc_sms_send()`
from it is refused **and** the syscall to reach the tty is unavailable — belt and
braces, at the two layers that are each independently sufficient.

This is also the point at which `/NeoDCT/User/apps` can safely start existing.

### Phase 4 — SELinux, only if Phase 2 proves insufficient

`SECURITY.md`'s memory analysis stands: refpolicy is far too large, and a
hand-written ~15-domain policy is a real project. Its one clear win over Landlock
is granular device `ioctl` control. **Do not start here.** Revisit once Phases
1–3 are shipped and you know what is still missing.

---

## 7. The browser specifically

You want `file://` restricted to `/NeoDCT/User/sdcard/webdata`. Three ways, and
they compose:

1. **Turn the file fetcher off entirely** if you do not need local browsing:
   drop `fetchers/file` from `content/fetchers/Makefile`. Smallest, most certain,
   and removes a chunk of parsing code from the largest attack surface on the
   phone.
2. **Landlock the browser process** in `apps/Browser/main.c` before it forks
   `netsurf-fb` (`nd_proc_spawn`): read-only on the webdata directory,
   read-write on its cache, nothing else. This is strictly better than a NetSurf
   patch because it constrains the *whole process* — a NetSurf RCE is caged too,
   not just its URL handling. Same mechanism as Phase 2, on the app that most
   needs it, and it can land first.
3. A NetSurf-side path check as defence in depth. Cheapest to write, easiest to
   bypass with `..` or a symlink; do not let it be the only measure.

Recommended: **1 if you can live without it, otherwise 2.**

One thing here is already right and is worth not undoing by accident:
**JavaScript is off on both targets.** `NETSURF_NEODCT_CONFIGURE_CMDS`
(`buildroot/package/netsurf/netsurf.mk:109`) sets `NETSURF_USE_DUKTAPE := NO`,
and because it is appended **last** in `NETSURF_CONFIGURE_CMDS` it overrides the
`YES` from the generic branch above it. That removes an entire interpreter
executing attacker-supplied programs as root, and it is the single largest
reason the browser is not already the worst thing on this phone. If a future
change reorders those `configure` hooks, JS comes back on silently — worth a
line in the build's acceptance check.

---

## 8. Three things not to do

- **Do not split `libneodct.so` for security.** Section 5. It costs RAM and buys
  nothing.
- **Do not ship `/NeoDCT/User/apps` before Phase 2.** Right now an installed app
  is native code running as root with no policy. The absence of that directory is
  currently load-bearing.
- **Do not start with SELinux.** It is the most memory, the most work and the
  most policy to get wrong, and Landlock plus a non-root UI gets you most of the
  distance for a fraction of both.

---

## 9. Findings summary

Status as of `SECURITY-PLAN.md` section 7. **Built** means implemented and
covered by host tests; nothing here has been booted.

| # | Finding | Severity | Phase | Status |
| --- | --- | --- | --- | --- |
| 1 | Update applier verifies no signature; verity root hash is attacker-writable | **Critical** | 0 | **Built.** The initramfs carries a verifier and the release key, and every field of `pending.prop` is compared against the signed manifest |
| 2 | `.remote/state.prop` is a self-installing persistent reverse-shell backdoor | **Critical** | 0/1 | Partly. `.remote` is 0700 `ndusr` and hidden from the browser, so it is out of reach of the untrusted set — but nd-core is still root, so a compromised app still reaches it |
| 3 | `env.sh` sourced as root on every boot from writable storage | **Critical** | 0 | **Built.** Gated on the kernel cmdline or a rootfs marker, and deliberately not on engineering mode |
| 4 | Everything runs as root; no MAC, no seccomp, no capability drop | High | 1/2 | Partly. Two users, and the browser is the untrusted one. nd-core, nd-apprun and the apps are still root; no seccomp |
| 5 | Browser reads the entire filesystem via `file://`, as root (JS is already off) | High | 0 | Partly. Not as root, and a mount namespace empties the engineering tree and the databases for it. The `file` fetcher is still built |
| 6 | No permission model; manifests have no permission field | High | 3 | Not started, deliberately — section 3 of the plan says doing it before the direct path is closed produces "a permission system that politely asks" |
| 7 | Apps can drive the modem directly; `nd_svc` narrowness is unenforced | High | 2 | Partly. The browser cannot: `ndusr_ut` is not in `dialout`. Apps still can, because nd-apprun is still root |
| 8 | `engineering_mode` is a writable flag unlocking shell, raw AT and Downgrade | Medium | 1 | Open. It is now explicitly refused as a gate where a gate has to be trustworthy (`env.sh`, the unsigned-update hatch) |
| 9 | User partition, `/tmp`, `/dev/shm` and auto-mounted SD cards lack `nosuid,nodev,noexec` | Medium | 0 | **Built**, with `noexec` where it belongs — the arrival partition and the boot-time card mount, not `/tmp` |
| 10 | SMS text and SNTP replies parsed on the core's own threads | Low (High once MMS lands) | 3 | Open |
| 12 | SNTP accepts any reply without matching the origin timestamp; clock is a TLS validity input | Low | 0 | Open |
| 11 | squashfs built without xattrs, so labelling is impossible later | Low | 0 | **Built.** `CONFIG_SQUASHFS_XATTR`; mksquashfs was already storing them |
| 13 | NetSurf pinned at 3.11 and forked, so its parsers cannot be bumped by a version change | Medium | 0 | Open. Its blast radius is smaller: a NetSurf RCE is now `ndusr_ut` in a mount namespace |
| 14 | Hand-written parsers eat untrusted bytes and are never fuzzed; `nd_json.c` runs *before* signature verification | Medium | 0 | Open in the app. In the initramfs the order is now the other way round: the signature is checked first and nothing parses the manifest until it passes |

One finding not in the original list, found while implementing: the SD card
had no way to express ownership at all, because a FAT filesystem has none and
mount options apply to a whole filesystem. Section 1 Option C of the plan is
the answer and it is built — a NeoDCT card is two FAT32 partitions now.

---

## 10. Obscurity, and what it is actually buying you

Two users, one of them on QEMU, neither using it as a main device, and nobody
writing malware for "NeoDCT OS". That is a real defence and it deserves to be
counted honestly rather than dismissed. But it does not apply evenly, and the
line it falls along is not the one people expect.

**Obscurity is a property of attacker economics, not of code.** It protects
against anything that has to *choose* you. It does nothing against anything that
arrives without knowing what it hit.

### What it genuinely buys

Quite a lot, and more than the userbase alone suggests:

- **No app store and no user-installable app path** (section 4, Q6). The single
  largest malware vector on every real phone does not exist here.
- **sshd binds `127.0.0.1`.** There is no inbound network service at all. An
  attacker cannot reach this phone; the phone has to reach them.
- **Nobody has NeoDCT to test against.** Exploits are brittle. Even a bug that is
  genuinely present needs offsets, and nobody has an image to derive them from.
- **The userbase is two people who are not interesting targets.** Targeted attack
  probability is, honestly, about zero.

### What it does not touch

Everything reachable **without knowing what NeoDCT is**:

- **A hostile web page.** libpng does not know what distro it is running on. A
  malicious image or CSS file works the same in NetSurf on this phone as
  anywhere else, and NetSurf here runs as root with the whole filesystem.
- **An SMS from anyone who has the number.** Zero-click by definition, and it
  reaches `nd_modem_at.c`'s parser without the sender knowing anything.
- **Physical access.** SD cards auto-mount `rw,suid,dev` on insertion, the serial
  console is a root login with no password (`AGENTS.md`, "Hardware access"), and
  `authorized_keys` is loadable straight off a card (section 4, Q5, vector 4).
- **You.** The most likely cause of data loss on this phone is a bug in it.

And note what obscurity does to the three criticals in section 9: **nothing.**
`env.sh`, `.remote/state.prop` and the update path are not entry points, they are
*amplifiers* — they turn one moment of code execution into permanent ownership.
Obscurity gates the entry; it has no effect on the outcome once something is
through. A single browser bug becomes a permanent backdoor regardless of how few
people run this OS.

### The inversion worth knowing about

The kernel is a standard Linux 6.12.x, which is the opposite of obscure. But the
*reachable* part of it here is much smaller than that sounds, and for a reason
that reads like a joke:

**Because everything already runs as root, local privilege-escalation bugs are
worthless against this phone.** There is nothing to escalate to. The entire
category of kernel LPE — historically most kernel CVEs that matter on a phone —
is inert here.

That stops being true the moment Phase 1 lands and `ndusr` exists. Dropping root
is still unambiguously the right move; it just means the kernel's LPE surface
becomes relevant on the same day, and the kernel point release starts mattering
in a way it currently does not.

What is reachable today is narrow: the network stack, the USB/modem paths, and
whatever the browser's syscalls touch. Small, but it is the part obscurity does
not cover.

### Where the bundled code actually stands

Measured, not assumed. Buildroot is **2025.11** and the pins are current:

| | |
| --- | --- |
| OpenSSL 3.6.0 · OpenSSH 10.2p1 · libcurl 8.17.0 | current |
| libpng 1.6.53 · jpeg-turbo 3.1.2 · freetype 2.14.1 | current |
| zlib 1.3.1 · sqlite 3.51.1 · expat 2.7.3 · busybox 1.37.0 | current |
| **NetSurf 3.11** | **2023, and forked** |

So the dependency hygiene is good, with one exception — and the exception is the
one that eats attacker-controlled bytes. NetSurf 3.11 brings its own
libcss/libdom/hubbub/libnsgif/libnsbmp, and because `netsurf-neodct/` is a
**fork** (22 MB, 418 `.c` files, replaced framebuffer frontend and libnsfb),
bumping it is a merge rather than a version bump. That cost compounds: the longer
the fork sits, the more expensive the security bump becomes.

Worth doing now, while the divergence is small enough to be remembered: rebase
the fork onto current NetSurf, and write down what was changed and why, so the
next bump is mechanical.

### The part obscurity makes *worse*

This is the half that gets missed. Obscurity means **nobody else is looking at
the code you wrote**, and that is precisely the code with the fewest eyes on it.

NetSurf's parsers have had years of fuzzing and thousands of readers. These have
had one:

| Module | Lines | Fed by |
| --- | --- | --- |
| `lib/nd_json.c` | 1462 | app manifests, **and update manifests** |
| `lib/nd_id3.c` | 711 | any MP3 copied onto an SD card |
| `lib/nd_modem_at.c` | 600 | modem output, including SMS text from strangers |
| `lib/nd_props.c` | 728 | settings, staging records, `verity_state.prop` |
| `lib/nd_manifest.c` | 503 | update packages |

The image decoders are *not* on this list, correctly: `nd_png.c` and `nd_jpeg.c`
are thin wrappers over libpng and libjpeg, which is the right call and worth not
undoing.

One ordering detail deserves attention. `apps/Update/main.c` opens the package,
parses `manifest.json`, checks compatibility, and **only then** verifies the
signature — an ordering three of the Python's tests depend on. So **`nd_json.c`
parses bytes from an unsigned, unverified package.** The signature check cannot
protect the parser that runs before it.

The mitigation is cheap and the machinery is already built: `make ASAN=1 test` is
in the acceptance gate (`tools/verify-c-build.sh:179`), so a fuzz target is a
`main()` that reads stdin into a buffer and calls one function. There is no
`fuzz` anywhere in the tree today. Three targets — `nd_json`, `nd_id3`,
`nd_modem_at` — would be an afternoon and would cover the surface obscurity most
conspicuously does not.

### The summary

Obscurity is doing real work, mostly by removing entry points rather than by
being unknown. It is also a **decaying asset**: the first popular release, or the
first HN post, spends it all at once — and security debt is far cheaper to pay
before that than after. The two things worth doing about it are not "assume
obscurity holds" or "assume it does not", but: **fix the amplifiers**, because
they convert any single bug into a permanent one, and **fuzz the code only you
have read**, because that is the part the outside world's attention has never
covered.

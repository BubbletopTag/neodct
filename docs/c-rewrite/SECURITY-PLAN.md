# Securing NeoDCT — a plan for the kernel we actually have

Third document in the set. `SECURITY.md` is a design conversation about a
future; `SECURITY-AUDIT.md` measures the present. This one is the bridge: what
to build, in what order, on **the kernel that ships on the phone** rather than
the one in the emulator.

It exists because the audit's Phase 2 does not survive contact with the
hardware, and because the shape of the fix changes completely once that is
admitted.

---

## 0. The correction that reorders everything

`SECURITY-AUDIT.md` §6, Phase 2 opens:

> Kernel is 6.12.47, so Landlock is available and needs no policy files, no
> xattrs and no daemon.

That is true of **QEMU only**.

| Target | Kernel | Source |
| --- | --- | --- |
| QEMU aarch64-virt | **6.12.47** | `buildroot/configs/neodct_qemu_defconfig:57` |
| Luckfox Pico Mini B (RV1103) | **5.10.110** | Rockchip SDK; `docs/HARDWARE_NOTES.md:235` |

**Landlock landed in Linux 5.13.** It does not exist on 5.10 and cannot be
backported into a vendor BSP kernel by a config option. So a confinement design
built on Landlock would pass every test in QEMU and protect the actual phone not
at all — which is the worst possible outcome, because it would *look* finished.

Everything below targets 5.10 and treats QEMU as the weaker constraint. Where
6.12 offers something better, it goes in as an addition for the QEMU target and
never as the mechanism the design depends on.

### What 5.10 does have

| Mechanism | On 5.10? | Needs |
| --- | --- | --- |
| DAC — users, groups, mode bits | **always** | nothing |
| `no_new_privs` | **always** | nothing |
| Capability bounding set | **always** | nothing |
| seccomp-bpf | yes, since 3.5 | `CONFIG_SECCOMP_FILTER` |
| Mount namespaces | yes | `CONFIG_NAMESPACES`, `CONFIG_MNT_NS` |
| `dm-verity` root-hash signature | yes, since 5.4 | `CONFIG_DM_VERITY_VERIFY_ROOTHASH_SIG` |
| SELinux | yes | xattr-capable squashfs **and** a policy |
| **Landlock** | **no** | 5.13+ |

The good news is that the audit's single most important fix — signing the verity
root hash so `installed.prop` stops being a trust input — **is available on
5.10**. The critical finding is fixable on the hardware today.

The bad news is that the thing the audit picked to replace SELinux is gone, so
the filesystem-confinement job falls to DAC and mount namespaces. That is
exactly the design proposed below, and it is why the two-user proposal is not a
stopgap: on this kernel it is the primary mechanism.

---

## 1. Two users, and why that is the right primitive here

The proposal — `ndusr` for the UI and trusted system apps, `ndusr_ut` for the
browser, MediaWidget and anything user-installed — is correct, and it is
*more* correct on 5.10 than it would be on 6.12.

Reasoning: DAC is the only confinement mechanism on this list that needs no
kernel configuration at all. It cannot be missing from the SDK kernel, it cannot
be compiled out, and it does not need a single line of the Rockchip BSP to
cooperate. Everything else in the table above is a config option somebody has to
have enabled. On a vendor kernel we do not control, "works with no kernel
support" is worth more than "more expressive".

### The mode-bit design for "cannot list anything but its own directory"

The requirement — `ndusr_ut` may not enumerate anything except
`/NeoDCT/User/browser/` — is achievable with mode bits alone, but it turns on a
detail that is easy to get wrong.

**Traversal and listing are different bits.** `x` on a directory permits
resolving a path *through* it; `r` permits reading the list of names in it. So
the user's own phrasing — *denied listing* — is precisely what the bits express:

```
/NeoDCT/User           0751  ndusr:ndusr        o+x, o-r   traverse, cannot list
/NeoDCT/User/browser   0770  ndusr:ndusr_ut     group rwx  ut's state, not its data
/NeoDCT/User/db        0750  ndusr:ndusr        o-x        ut cannot even enter
/NeoDCT/User/.remote   0700  ndusr:ndusr                   nor this
/NeoDCT/User/.ndsys    0700  ndusr:ndusr                   nor this
```

`0751` on the parent is load-bearing and slightly counter-intuitive: it is what
lets `ndusr_ut` reach `browser/` by name while `ls /NeoDCT/User` returns
`EACCES`. Everything sensitive is then protected by removing `o+x` on *its own*
directory, not by hiding the path — because a path that is guessed still fails
at the directory that denies entry.

`ndusr_ut` must **not** be in group `ndusr`. The group is what separates
"trusted apps share the user's data" from "the browser does not".

### Where untrusted data actually goes: the card, not the partition

`/NeoDCT/User` is **8 MiB** on the Luckfox — `docs/PARTITIONS.md:27`, a UBI
volume on a 128 MiB NAND chip. That is enough for settings, a phonebook and an
SMS database and nothing else. It is not a download directory, and treating it
as one is how the *phone* breaks rather than the browser: settings, the message
databases and the call log all write there, so a browser that fills the
partition takes the rest of the system down with it.

So the untrusted working directory is on the card, with the partition as a
fallback:

```
/NeoDCT/User/sdcard/untrusted/   primary  downloads, MMS attachments, anything big
/NeoDCT/User/browser/            fallback cookies and state when no card is present
```

The split is by *size class*, not by trust: both are untrusted, and the small
one exists so the browser still works with no card in the slot.

### The problem this creates: FAT has no ownership

**A FAT filesystem has no ownership and no mode bits.** `chown` and `chmod` do
not work on it. Permissions come from mount options — `uid=`, `gid=`, `fmask=`,
`dmask=` — and they apply to **the whole filesystem at once**.

Today the card is mounted `rw,noatime,utf8,flush`
(`System/hw/neodct-sdcard:177`) with no ownership options at all, so everything
on it belongs to whoever mounted it.

The consequence for this design is unavoidable and worth stating plainly:
**there is no way to grant `ndusr_ut` `sdcard/untrusted/` and deny it
`sdcard/music/` using permissions.** One mount, one uid, one gid. If the card is
mounted for `ndusr_ut`, `ndusr_ut` has the entire card — including any
`authorized_keys` sitting on it, which `SECURITY-AUDIT.md` §4 Q5 vector 4
already flags as loadable straight off a card.

There are three ways out. The third is the one to take, and it was missed on the
first pass because the constraint was read as "FAT cannot express ownership"
when the accurate version is "**a FAT filesystem** cannot express ownership" —
and a card can carry more than one of them.

#### Option A — keep FAT, bind-mount the subtree

The only mechanism that expresses "this subtree and nothing else" on a
filesystem with no permissions is a **bind mount inside a mount namespace**:

```
core (ndusr):  mounts the card once, uid=ndusr, noexec,nosuid,nodev
launcher:      unshare(CLONE_NEWNS)
               bind /NeoDCT/User/sdcard/untrusted -> the only card path ut sees
               drop to ndusr_ut
               exec the untrusted binary
```

Keeps the card readable on any computer. Makes the namespace a prerequisite
rather than an improvement, and therefore makes `CONFIG_MNT_NS` in a vendor BSP
kernel load-bearing.

#### Option B — format the card ext, and use ordinary permissions

An ext filesystem has real ownership, so the whole problem disappears:

```
/NeoDCT/User/sdcard              0751  ndusr:ndusr
/NeoDCT/User/sdcard/untrusted    0770  ndusr:ndusr_ut
/NeoDCT/User/sdcard/music        0750  ndusr:ndusr
```

Most of the machinery for this already exists and was found rather than
assumed:

| | |
| --- | --- |
| `CARD_FSTYPES="vfat exfat ext4 ext3 ext2"` | ext cards **already mount** (`neodct-sdcard:162`) |
| the `*)` branch already passes `rw,noatime` | the FAT-only `utf8,flush` are correctly excluded |
| `neodct-sdcard format DEV` already exists | the phone can format its own cards (`:239`) |
| busybox `CONFIG_MKE2FS=y` | **an ext formatter is already in the image** — no new package |

So the change is roughly one line in `do_format()` — `mkfs.vfat -F 32` becomes
the ext equivalent — plus creating the directory tree with the right ownership
while the phone still has it mounted.

**What it costs, stated honestly, because it is not free:**

1. **Interoperability, which is the real price.** A FAT card reads on any
   computer. An ext card does not read on Windows or macOS without third-party
   software. The entire sideloading workflow depends on this — Settings' own
   help text tells the owner to *"Format a card as FAT32, make a folder called
   wallpapers on it"* (`apps/Settings/main.c:92`). Wallpapers, music and tones
   all arrive this way.
2. **ext can express things FAT structurally cannot: setuid bits and device
   nodes.** A card is removable and its contents are chosen by whoever had it
   last. On FAT, `nosuid,nodev` is close to moot because the filesystem cannot
   represent either. On ext it becomes **mandatory** — `SECURITY-AUDIT.md`
   finding 9 stops being hygiene and becomes the thing standing between a
   crafted card and a root shell. Switching to ext makes that mount option
   load-bearing where it currently is not.
3. **Numeric uids live on the medium.** ext stores uid 1001, not "ndusr_ut". A
   card formatted anywhere else carries meaningless numbers. Mitigated by the
   phone owning the formatting — which it already can — but that makes
   "format on the phone" a requirement rather than a convenience.
4. **Journalling on cheap flash, on a device that loses power without warning.**
   Worth choosing deliberately: ext2 (no journal, and what busybox's `mke2fs`
   produces) or ext4 with the journal disabled.

#### Option C — two FAT32 partitions, which is the recommendation

The ownership options FAT *does* have — `uid=`, `gid=`, `fmask=`, `dmask=` —
apply per **mount**, and a mount is per **block device**. A partitioned card is
two block devices. So two partitions get two genuinely independent sets of
ownership options, with no kernel feature and no filesystem change:

```
p1  FAT32  uid=ndusr,    gid=ndusr,    dmask=0027,fmask=0137, nosuid,nodev
           music, wallpapers, tones -- the import side

p2  FAT32  uid=ndusr_ut, gid=ndusr_ut, dmask=0077,fmask=0177, nosuid,nodev,noexec
           downloads, MMS attachments -- the untrusted side
```

This dominates the other two:

| | A (namespace) | B (ext) | **C (two partitions)** |
| --- | --- | --- | --- |
| Needs an SDK kernel option | **yes** | no | **no** |
| Card reads on a PC | yes | **no** | **yes** |
| Needs a format | no | **yes** | yes (repartition) |
| Can carry setuid / device nodes | no | **yes** | **no** |

Note the last row, which is not obvious: staying on FAT keeps the property that
a crafted card *structurally cannot* carry a setuid binary or a device node.
Option B hands that away in exchange for ownership; C gets the ownership and
keeps the property.

`candidates()` already enumerates partitions before whole disks
(`neodct-sdcard:145`), so the device discovery is done. What is missing is that
the helper mounts **the first partition that works and returns** — it would need
to mount both, and `do_format()` would need to lay down a partition table rather
than a superfloppy. Both are contained changes to one shell script.

The split is also a better fit for what the card is *for*: the media the owner
copies on from a PC and the junk the browser downloads were never the same kind
of data, and this makes that a boundary rather than a convention.

#### Recommendation

**C.** It needs nothing from the vendor kernel, keeps the card readable on any
computer, and keeps FAT's inability to express setuid — which turns out to be a
security property worth holding on to.

The owner's warning is still worth showing, because repartitioning still wipes
the card. It just no longer has to say "this card will not read on a PC any
more".

**A remains viable** now that the vendor kernel's config is known to be
changeable — `CONFIG_MNT_NS` would make the bind-mount route work on a
single-partition FAT card. It is the fallback if partitioning turns out to be
awkward on the phone, not the plan.

**B is the one to drop.** Its only advantage over C was ownership, which C also
has, and it pays for it with interop and with the setuid property.

---

**A correction to the previous revision.** It offered, as A's fallback, "a
second FAT mount of the same card with different `uid=`/`fmask=` options". That
is unsound: mounting one block device twice generally shares the superblock and
the second mount's options are ignored rather than applied. Two *partitions* is
the version of that idea that actually works, and it is Option C.

#### What holds regardless of which option wins

- **A foreign, single-partition FAT card must still mount** for importing music
  and wallpapers. That direction is the one the owner actually uses, and it is
  read-mostly.
- **The untrusted directory is only offered on a NeoDCT-formatted card.** On a
  foreign card there is no `ndusr_ut` partition, so untrusted state falls back
  to `/NeoDCT/User/browser/` and downloads are **refused with a clear message**
  rather than silently filling an 8 MiB partition the rest of the phone writes
  to.
- **Warn before formatting.** Repartitioning wipes the card under any option, so
  the format screen should say so plainly.
- **`nosuid,nodev` on every card mount, `noexec` on the untrusted one.** Under
  C this is defence in depth, because FAT cannot represent setuid anyway. Under
  B it is the only thing between a crafted card and a root shell. Write it once,
  now, and it is correct under either.
- **None of this removes the namespace from the plan** — it is still wanted for
  the minimal `/dev` of §2 and for keeping `file:///` out of `/NeoDCT/System`.
  What C removes is storage's *dependency* on it, which is the part that was
  forcing the phase order.

### Where DAC stops, and what to add

DAC cannot protect the parts of the image that are legitimately world-readable.
`/NeoDCT/System` has to be readable by every app, so `file:///NeoDCT/System/...`
in the browser still enumerates the whole system tree, and
`SECURITY-AUDIT.md` finding 5 is only half-fixed.

Two answers, in order of cost:

1. **Turn off NetSurf's `file` fetcher** (`content/fetchers/Makefile`). Costs
   nothing, removes a parser, and is what the audit recommends — but the stated
   plan is to *use* `file:///` for MMS attachments later, so this is a
   deferral, not a fix.
2. **A mount namespace for `ndusr_ut`.** `unshare(CLONE_NEWNS)` in the launcher,
   then build the untrusted process a root containing only what it needs. This
   is the 5.10 stand-in for Landlock and it is strictly more thorough: it
   removes paths from *existence* rather than denying access to them, so there
   is no `..`, no symlink and no `/proc/self/root` trick to walk back out.

The MMS directory named in the proposal fits (2) exactly: bind-mount the one
temporary directory holding the attachment being displayed into the namespace,
read-only, and unmount it when the viewer exits. It is present while it is
needed and it does not exist the rest of the time — which is a much stronger
statement than any permission bit.

**Verify before designing on it:** the SDK kernel must have `CONFIG_NAMESPACES`
and `CONFIG_MNT_NS`. Both are near-universal, but this is a vendor BSP and the
whole point of §0 is not to assume. `zcat /proc/config.gz` on the phone, or
`grep MNT_NS` in the SDK's `.config`, settles it in one command.

---

## 2. The part that has to be solved first: foreign binaries open devices by path

This is the obstacle the plan lives or dies on, and it is not obvious from the
audit.

**Native NeoDCT apps already have the abstraction being asked for.** `nd-apprun`
opens the framebuffer and the key channel and passes them down as inherited
descriptors:

```
NEODCT_FB_FD       the framebuffer, already open        nd_app.h:135
NEODCT_KEYPAD_FD   the key channel, already open        nd_app.h:133
```

`SECURITY-AUDIT.md` counts this as a thing already done right, and it is: a
native app needs **no `/dev` permission whatsoever** to draw and to read keys.
For Calculator, Clock, Calendar and every other stock app, `ndusr_ut` could be
given an empty `/dev` today and nothing would break.

**The foreign binaries do not participate.** Three of them, and they are exactly
the three the proposal names as untrusted:

| Binary | Opens | Consequence if `ndusr_ut` may open it |
| --- | --- | --- |
| `netsurf-fb` | `/dev/fb0` directly (`apps/Browser/main.c:14`) | can draw anywhere on the panel, including over trusted UI |
| `netsurf-fb` | `/dev/input/*` itself | **reads every keypress on the phone** |
| `mpv` | ALSA, `/dev/fb0` | the screen |
| `aplay` | ALSA | audio only; the mild one |

The second row is the serious one. Read access to the real evdev node is a
keylogger — it does not matter which app is in the foreground, an evdev reader
sees everything typed, including into the dialler and any future PIN entry.

### MMS goes through MediaWidget, not the browser

Worth correcting an assumption it is easy to make: **MMS attachments will be
displayed by MediaWidget — `mpv` — not by NetSurf.** That is why `mpv` belongs
in the untrusted set alongside the browser, and it changes where the risk sits.

`mpv` is a *larger* parser surface than the browser is for this purpose, not a
smaller one. NetSurf with JavaScript off parses HTML, CSS and a handful of image
formats. `mpv` brings a demuxer for every container it was built with and a
decoder behind each — and an MMS attachment is bytes chosen by a stranger who
needs only the phone number.

Three things the current MediaWidget already does that turn out to be security
properties rather than performance ones, and which must not be undone:

| Flag | Why it matters now |
| --- | --- |
| `--no-config` (`nd_media.c:144`) | no config file on the card can change how the decoder behaves |
| `--sub-auto=no` (`:163`) | mpv does not stat the directory around the file it was given |
| tiny demuxer cache (`:177`) | bounds what a hostile file can make it allocate |
| **no Lua in the build** (`:165`) | the scripting layer that would execute attacker-chosen code is absent entirely, so the options controlling it do not even exist |

The last one is the strongest and the least visible — it is the mpv equivalent
of NetSurf shipping with JavaScript off, and like that one it is a build
property nobody would notice regressing. Both deserve a line in the build's
acceptance check.

**The copy-before-display step is the right instinct and worth making precise.**
Copying the attachment into the untrusted directory before handing it to `mpv`
means `mpv` never opens the message database's directory at all. To get the full
value from it:

- **Generate the filename.** Never reuse the one the sender supplied — it is
  attacker-controlled text and the only thing it needs to survive is being
  opened. A sequence number and the MIME type's extension is enough.
- **Per-message directory, `0700 ndusr_ut`, removed on exit.** Bind-mounted into
  the namespace while the viewer runs and gone afterwards, so the window in
  which the file is reachable is the window in which it is on screen.
- **Hand `mpv` a path inside its namespace, or an fd.** It should never need to
  enumerate anything to find its own attachment.
- The card is `noexec` (§1), which is what stops a "video" that is really an ELF
  from being one command away from running.

### The mitigation the hardware hands us for free

On the Luckfox the keypad is **not an evdev device**. It is an i2c expander the
core scans, and the core synthesises a uinput device for the browser's arrow
keys (`apps/Browser/main.c:1020`). So on real hardware, if `/dev/input` contains
only the synthetic bridge, `netsurf-fb` sees the keys the core chose to give it
and nothing else.

In QEMU the keypad *is* `/dev/input/event0`, so the emulator is the more
dangerous configuration here — the reverse of the usual. Worth knowing before
concluding from a QEMU test that the browser is contained.

### Three routes, and the recommendation

1. **Mount namespace with a minimal `/dev`** — `/dev/fb0`, the uinput bridge,
   `/dev/null`, `/dev/urandom`, the ALSA nodes. Nothing else exists.
   No patches to foreign software; works on 5.10; one mechanism serves the
   filesystem confinement in §1 as well. **Recommended.**
2. **Teach `netsurf-fb` to take an inherited fd**, the way native apps do. This
   is the architecturally pure answer and it matches the existing pattern, but
   it is a patch to a forked 22 MB tree the audit already flags as expensive to
   maintain (finding 13).
3. **DAC on the device nodes** — group `video`/`input`, with `ndusr_ut` out of
   them. Cheapest, and it fails for the browser specifically, which genuinely
   needs the framebuffer. Useful for `mpv`; not sufficient for NetSurf.

Route 1 subsumes 3 and defers 2. Do 1.

### The trade that has to be stated rather than glossed

While the browser is on screen, it owns the screen. There is no compositor on a
64 MB phone and there will not be one, so a full-screen handoff means the
untrusted process can draw anything, including a convincing imitation of the
trusted UI. That is inherent, not a bug to be fixed later.

What keeps it bounded: it cannot run in the background, the core repaints on
exit, and it never sees a keypress the core did not route to it. Say this out
loud in `SECURITY.md` rather than letting a reader assume the browser is boxed
in more tightly than it is.

---

## 3. Hardware abstraction as the place permissions get decided

The proposal's reasoning — *abstraction will allow us to eventually control what
apps have access to what, and `ut` can be denied* — is the right one, and it is
worth being precise about **why** it works here, because `SECURITY-AUDIT.md` §5
correctly warns that a library is not a boundary.

Both are true, and they are not in conflict:

- A library is **not** a boundary. An app denied `libneodct.so` writes ten lines
  of `termios` and talks to `/dev/ttyUSB2` itself. The linker enforces nothing.
- A library **is** the right place to put the decision, *once something else
  makes the direct path impossible.*

So the order matters, and it is the opposite of the intuitive one:

```
1. remove the direct path      mount namespace: /dev/ttyUSB2 does not exist for ut
2. THEN the abstraction is the only path
3. THEN a check inside it is enforcement rather than a suggestion
```

Step 1 is what turns `nd_svc.h`'s four operations from a convenience API into a
boundary — which is exactly what the audit says is missing. Step 3 is where the
per-app permission field eventually reads.

Doing 3 before 1 produces a permission system that politely asks. Doing 1 first
means every subsequent permission question is answered in ordinary C that can be
read and tested.

---

## 4. Bare minimum packages

Measured against `buildroot/configs/neodct_qemu_defconfig`, which enables **36**
`BR2_PACKAGE_*` entries. Each candidate below was checked by grepping the whole
tree for an actual invocation, excluding `t9.dict` — which is an English
wordlist and matches "links", "curl" and "gpm" as ordinary words.

### Remove — high confidence

| Package | Finding |
| --- | --- |
| `LINKS`, `LINKS_GRAPHICS` | **A second graphical web browser, never launched.** Every apparent reference in the tree is the English verb ("OpenSSH links it", "follow links"). The only real `links` is `netsurf-neodct/tests/fb-browser/links-2.30/`, a **test fixture** — a tarball and its extracted source — not the shipped browser. This ships a complete second HTML/CSS/image parser and network client, as root, that nothing can reach. Largest single attack-surface reduction available. |
| `GPM` | A mouse daemon on a phone with no pointer. Its only appearance in the tree is inside that same `links-2.30` fixture — `LINKS_GRAPHICS` is what pulls it in. Removing LINKS should remove the reason for GPM; verify it is not also a NetSurf framebuffer input dependency before dropping. |
| `ALSA_UTILS_ALSAMIXER` | The interactive ncurses mixer. **No reference anywhere.** `amixer` — the scriptable one, used by `S17audio`, `nd_btaudio.c` and `MusicPlayer/audio.c` — is a separate sub-option and stays. |

### Remove — worth confirming first

| Package | Finding |
| --- | --- |
| `MPG123` | Live code, unreachable path. `koki_audio.c:641` probes for it, but the selection order is override → `mpv` → `mpg123`, and `mpv` is unconditionally in the image, so the `mpg123` rung can never be taken. Removing it costs a fallback that cannot fire. Keep if the intent is that `mpv` may one day be dropped. |
| `DOSFSTOOLS_MKFS_FAT` | Referenced only by `neodct-sdcard:249`'s `do_format()`. Under §1 Option B that line becomes an ext formatter, and busybox's `CONFIG_MKE2FS=y` already supplies one — so this sub-option leaves with it. `FSCK_FAT` and `FATLABEL` stay while foreign FAT cards are still mounted for importing media. |

### Keep — and why, so nobody removes them later

| Package | Reason |
| --- | --- |
| `LVM2` | Looks unused — no `lvm` reference anywhere — but it provides **`dmsetup`**, which `initramfs/init` uses to build the dm-verity mapping. Removing it un-boots the phone. Worth a comment in the defconfig; this is the trap in this list. |
| `MPV` | Koki's sfx fallback and the MediaWidget. |
| `UQMI` | `S45modem`. |
| `LIBCURL` | `S45modem`, update downloader. |
| `OPENSSH_*` | The remote-shell debugging feature. |
| `BLUEZ5_*`, `BLUEZ_ALSA` | `nd_btaudio.c`, `bluetoothctl`. |
| `CA_CERTIFICATES`, `FREETYPE`, `JPEG`, `DEJAVU` | TLS and rendering. |

### How to prove "no regression"

The claim is testable rather than argued. Build with the candidates removed, then:

1. `make test` — 71 unit tests, several of which `which()` for these binaries.
2. Boot in QEMU and walk: browser opens a page, Koki plays sfx, MusicPlayer
   plays an MP3, Tones previews a ringtone, an SD card mounts and `fsck`s,
   Bluetooth pairs, a call rings.
3. Diff the image manifest before and after, so what actually left is a fact
   rather than an intention.

Do the removals **one commit each**. A single "shrink the image" commit that
turns out to have taken `dmsetup` with it is a bisect through a phone that does
not boot.

---

## 5. Order

Each phase is separately shippable and separately testable. Phases 0 and 1 are
worth doing whatever happens to the rest.

**Phase 0 — no new users, no kernel features.**
Sign-check in the initramfs before the `dd` (`SECURITY-AUDIT.md` §3 — the
critical one, and `CONFIG_DM_VERITY_VERIFY_ROOTHASH_SIG` is available on 5.10).
`nosuid,nodev` on `/NeoDCT/User`, `/tmp`, `/dev/shm` and SD mounts; `noexec` on
cards. Gate `env.sh`. Remove the dead packages in §4. None of this needs a user
to exist.

**Phase 1 — the two users, DAC only.**
`ndusr` and `ndusr_ut` exist; the mode-bit layout of §1; the core drops to
`ndusr` after opening the framebuffer; udev rules and group membership for the
devices the audit tabulates. `/NeoDCT/User/browser` is the untrusted directory
for now, because it is the one that works without a namespace. Stop here and the
browser still has `/dev/fb0`, but `ndusr_ut` can no longer read the SMS
database, the ssh keys or the settings — which is most of the value for a
fraction of the work.

**Phase 2 — the mount namespace. Required, not optional.**
`unshare(CLONE_NEWNS)` in the launcher for untrusted apps; a minimal `/dev`;
a bind mount of `sdcard/untrusted/` as the only card path that exists for
`ndusr_ut`.

Under **Option C in §1 (two FAT partitions), which is the recommendation**,
storage confinement is ordinary mount options and this phase is what it always
should have been: the way to give untrusted processes a minimal `/dev` (§2) and
to keep `file:///` out of `/NeoDCT/System`. Worth doing, not blocking.

Only Option A makes it a prerequisite, and only because a single FAT filesystem
has no ownership to attach a permission to.

The vendor kernel's config **is** changeable — it has been changed before, for
Bluetooth — so `CONFIG_MNT_NS` is no longer a bet. It is a task. That widens the
options rather than settling them: C is still preferable because a change that
needs no kernel rebuild ships faster and cannot regress when the SDK is next
updated.

**Phase 3 — seccomp, then the permission field.**
`no_new_privs` + a filter that denies `socket()` for apps that declare no
network, plus `ptrace`, `mount`, `reboot`. Then `permissions` in
`manifest.json`, read by `nd_ui_scan_apps()` and enforced in `nd_svc`.

**Not yet — SELinux.** The audit's advice stands and 5.10 does not change it.

---

## 6. What could not be verified from here

Stated plainly so nobody builds on it by accident:

- **`CONFIG_MNT_NS` in the SDK kernel — check this one first.** Two separate
  features now rest on it: untrusted data on the SD card (§1) and the MMS
  per-message bind mount (§2). `build-luckfox/` is not in this clone and the
  kernel is built in a distrobox from the Rockchip SDK, so it is **assumed
  present, not confirmed**. One command on the phone settles it:

  ```sh
  zcat /proc/config.gz | grep -E 'MNT_NS|NAMESPACES|SECCOMP_FILTER|VERITY'
  ```

  If `/proc/config.gz` is not built in, the SDK's own `.config` says the same.
- **`CONFIG_SECCOMP_FILTER` and `CONFIG_DM_VERITY_VERIFY_ROOTHASH_SIG`**, same
  caveat. Phase 3 and the Phase 0 verity fix depend on them respectively.
- **Whether `GPM` is only a LINKS dependency.** It may also be a NetSurf
  framebuffer input option; the netsurf package needs reading before it is
  dropped.
- **Nothing here has been built or booted.** This is a plan derived from reading
  the tree, not a change that has been tested.

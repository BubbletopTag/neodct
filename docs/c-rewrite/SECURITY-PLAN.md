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
/NeoDCT/User/browser   0770  ndusr:ndusr_ut     group rwx  ut's own space
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
| `DOSFSTOOLS_MKFS_FAT` | No reference. Cards are formatted on a PC per the Settings help text. `FSCK_FAT` and `FATLABEL` *are* referenced and stay. |

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
devices the audit tabulates. Stop here and the browser still has `/dev/fb0`, but
`ndusr_ut` can no longer read the SMS database, the ssh keys or the settings —
which is most of the value for a fraction of the work.

**Phase 2 — the mount namespace.**
`unshare(CLONE_NEWNS)` in the launcher for untrusted apps; minimal `/dev`;
`/NeoDCT/User/browser` as the only writable path. This closes `file:///`
properly and makes §3 step 1 true.

**Phase 3 — seccomp, then the permission field.**
`no_new_privs` + a filter that denies `socket()` for apps that declare no
network, plus `ptrace`, `mount`, `reboot`. Then `permissions` in
`manifest.json`, read by `nd_ui_scan_apps()` and enforced in `nd_svc`.

**Not yet — SELinux.** The audit's advice stands and 5.10 does not change it.

---

## 6. What could not be verified from here

Stated plainly so nobody builds on it by accident:

- **The SDK kernel's config.** `build-luckfox/` is not in this clone and the
  kernel is built in a distrobox from the Rockchip SDK, so `CONFIG_SECCOMP_FILTER`,
  `CONFIG_NAMESPACES`, `CONFIG_MNT_NS` and
  `CONFIG_DM_VERITY_VERIFY_ROOTHASH_SIG` are **assumed present, not confirmed**.
  Phases 2 and 3 and the Phase 0 verity fix each depend on one of them. Check
  before scheduling, not during.
- **Whether `GPM` is only a LINKS dependency.** It may also be a NetSurf
  framebuffer input option; the netsurf package needs reading before it is
  dropped.
- **Nothing here has been built or booted.** This is a plan derived from reading
  the tree, not a change that has been tested.
